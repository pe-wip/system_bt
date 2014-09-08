/******************************************************************************
 *
 *  Copyright (C) 2014 Google, Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#define LOG_TAG "hci_layer"

#include <assert.h>
#include <utils/Log.h>

#include "alarm.h"
#include "buffer_allocator.h"
#include "btsnoop.h"
#include "controller.h"
#include "fixed_queue.h"
#include "hcimsgs.h"
#include "hci_hal.h"
#include "hci_internals.h"
#include "hci_inject.h"
#include "hci_layer.h"
#include "list.h"
#include "low_power_manager.h"
#include "osi.h"
#include "packet_fragmenter.h"
#include "reactor.h"
#include "vendor.h"

#define HCI_COMMAND_COMPLETE_EVT    0x0E
#define HCI_COMMAND_STATUS_EVT      0x0F

#define INBOUND_PACKET_TYPE_COUNT 3
#define PACKET_TYPE_TO_INBOUND_INDEX(type) ((type) - 2)
#define PACKET_TYPE_TO_INDEX(type) ((type) - 1)

#define PREAMBLE_BUFFER_SIZE 4 // max preamble size, ACL
#define RETRIEVE_ACL_LENGTH(preamble) ((((preamble)[3]) << 8) | (preamble)[2])

static const uint8_t preamble_sizes[] = {
  HCI_COMMAND_PREAMBLE_SIZE,
  HCI_ACL_PREAMBLE_SIZE,
  HCI_SCO_PREAMBLE_SIZE,
  HCI_EVENT_PREAMBLE_SIZE
};

static const uint16_t outbound_event_types[] =
{
  MSG_HC_TO_STACK_HCI_ERR,
  MSG_HC_TO_STACK_HCI_ACL,
  MSG_HC_TO_STACK_HCI_SCO,
  MSG_HC_TO_STACK_HCI_EVT
};

typedef enum {
  BRAND_NEW,
  PREAMBLE,
  BODY,
  IGNORE,
  FINISHED
} receive_state_t;

typedef struct {
  receive_state_t state;
  uint16_t bytes_remaining;
  uint8_t preamble[PREAMBLE_BUFFER_SIZE];
  uint16_t index;
  BT_HDR *buffer;
} packet_receive_data_t;

typedef struct {
  uint16_t opcode;
  command_complete_cb complete_callback;
  command_status_cb status_callback;
  void *context;
  BT_HDR *command;
} waiting_command_t;

static const uint32_t EPILOG_TIMEOUT_MS = 3000;
static const uint32_t COMMAND_PENDING_TIMEOUT = 8000;

// Our interface
static bool interface_created;
static hci_t interface;

// Modules we import and callbacks we export
static const allocator_t *buffer_allocator;
static const btsnoop_t *btsnoop;
static const controller_t *controller;
static const hci_callbacks_t *callbacks;
static const hci_hal_t *hal;
static const hci_hal_callbacks_t hal_callbacks;
static const hci_inject_t *hci_inject;
static const low_power_manager_t *low_power_manager;
static const packet_fragmenter_t *packet_fragmenter;
static const packet_fragmenter_callbacks_t packet_fragmenter_callbacks;
static const vendor_t *vendor;

static thread_t *thread; // We own this

static volatile bool firmware_is_configured = false;
static volatile bool has_shut_down = false;
static alarm_t *epilog_alarm;

// Outbound-related
static int command_credits = 1;
static fixed_queue_t *command_queue;
static fixed_queue_t *packet_queue;

// Inbound-related
static alarm_t *command_response_alarm;
static list_t *commands_pending_response;
static pthread_mutex_t commands_pending_response_lock;
static packet_receive_data_t incoming_packets[INBOUND_PACKET_TYPE_COUNT];

static void event_preload(void *context);
static void event_postload(void *context);
static void event_epilog(void *context);
static void event_command_ready(fixed_queue_t *queue, void *context);
static void event_packet_ready(fixed_queue_t *queue, void *context);

static void firmware_config_callback(bool success);
static void sco_config_callback(bool success);
static void epilog_finished_callback(bool success);

static void hal_says_data_ready(serial_data_type_t type);
static void epilog_wait_timer_expired(void *context);

// Interface functions

static bool start_up(bdaddr_t local_bdaddr, const hci_callbacks_t *upper_callbacks) {
  assert(local_bdaddr != NULL);
  assert(upper_callbacks != NULL);

  ALOGI("%s", __func__);

  // The host is only allowed to send at most one command initially,
  // as per the Bluetooth spec, Volume 2, Part E, 4.4 (Command Flow Control)
  // This value can change when you get a command complete or command status event.
  command_credits = 1;
  firmware_is_configured = false;
  has_shut_down = false;

  pthread_mutex_init(&commands_pending_response_lock, NULL);

  epilog_alarm = alarm_new();
  if (!epilog_alarm) {
    ALOGE("%s unable to create epilog alarm.", __func__);
    goto error;
  }

  command_response_alarm = alarm_new();
  if (!command_response_alarm) {
    ALOGE("%s unable to create command response alarm.", __func__);
    goto error;
  }

  command_queue = fixed_queue_new(SIZE_MAX);
  if (!command_queue) {
    ALOGE("%s unable to create pending command queue.", __func__);
    goto error;
  }

  packet_queue = fixed_queue_new(SIZE_MAX);
  if (!packet_queue) {
    ALOGE("%s unable to create pending packet queue.", __func__);
    goto error;
  }

  thread = thread_new("hci_thread");
  if (!thread) {
    ALOGE("%s unable to create thread.", __func__);
    goto error;
  }

  commands_pending_response = list_new(NULL);
  if (!commands_pending_response) {
    ALOGE("%s unable to create list for commands pending response.", __func__);
    goto error;
  }

  callbacks = upper_callbacks;
  memset(incoming_packets, 0, sizeof(incoming_packets));

  controller->init(&interface);
  packet_fragmenter->init(&packet_fragmenter_callbacks);

  fixed_queue_register_dequeue(command_queue, thread_get_reactor(thread), event_command_ready, NULL);
  fixed_queue_register_dequeue(packet_queue, thread_get_reactor(thread), event_packet_ready, NULL);

  vendor->open(local_bdaddr, &interface);
  hal->init(&hal_callbacks, thread);
  low_power_manager->init(thread);

  vendor->set_callback(VENDOR_CONFIGURE_FIRMWARE, firmware_config_callback);
  vendor->set_callback(VENDOR_CONFIGURE_SCO, sco_config_callback);
  vendor->set_callback(VENDOR_DO_EPILOG, epilog_finished_callback);

  if (!hci_inject->open(&interface)) {
    // TODO(sharvil): gracefully propagate failures from this layer.
  }

  return true;
error:;
  interface.shut_down();
  return false;
}

static void shut_down() {
  if (has_shut_down) {
    ALOGW("%s already happened for this session", __func__);
    return;
  }

  ALOGI("%s", __func__);

  hci_inject->close();

  if (thread) {
    if (firmware_is_configured) {
      alarm_set(epilog_alarm, EPILOG_TIMEOUT_MS, epilog_wait_timer_expired, NULL);
      thread_post(thread, event_epilog, NULL);
    } else {
      thread_stop(thread);
    }

    thread_join(thread);
  }

  fixed_queue_free(command_queue, buffer_allocator->free);
  fixed_queue_free(packet_queue, buffer_allocator->free);
  list_free(commands_pending_response);

  pthread_mutex_destroy(&commands_pending_response_lock);

  packet_fragmenter->cleanup();


  alarm_free(epilog_alarm);
  alarm_free(command_response_alarm);

  low_power_manager->cleanup();
  hal->close();

  interface.set_chip_power_on(false);
  vendor->close();

  thread_free(thread);
  thread = NULL;
  firmware_is_configured = false;
  has_shut_down = true;
}

static void set_chip_power_on(bool value) {
  ALOGD("%s setting bluetooth chip power on to: %d", __func__, value);

  int power_state = value ? BT_VND_PWR_ON : BT_VND_PWR_OFF;
  vendor->send_command(VENDOR_CHIP_POWER_CONTROL, &power_state);
}

static void do_preload() {
  ALOGD("%s posting preload work item", __func__);
  thread_post(thread, event_preload, NULL);
}

static void do_postload() {
  ALOGD("%s posting postload work item", __func__);
  thread_post(thread, event_postload, NULL);
}

static void turn_on_logging(const char *path) {
  ALOGD("%s", __func__);

  if (path != NULL)
    btsnoop->open(path);
  else
    ALOGW("%s wanted to start logging, but path was NULL", __func__);
}

static void turn_off_logging() {
  ALOGD("%s", __func__);
  btsnoop->close();
}

static void transmit_command(
    BT_HDR *command,
    command_complete_cb complete_callback,
    command_status_cb status_callback,
    void *context) {
  waiting_command_t *wait_entry = osi_calloc(sizeof(waiting_command_t));
  if (!wait_entry) {
    ALOGE("%s couldn't allocate space for wait entry.", __func__);
    return;
  }

  uint8_t *stream = command->data + command->offset;
  STREAM_TO_UINT16(wait_entry->opcode, stream);
  wait_entry->complete_callback = complete_callback;
  wait_entry->status_callback = status_callback;
  wait_entry->command = command;
  wait_entry->context = context;

  // Store the command message type in the event field
  // in case the upper layer didn't already
  command->event = MSG_STACK_TO_HC_HCI_CMD;

  fixed_queue_enqueue(command_queue, wait_entry);
}

static void transmit_downward(data_dispatcher_type_t type, void *data) {
  if (type == MSG_STACK_TO_HC_HCI_CMD) {
    // TODO(zachoverflow): eliminate this call
    transmit_command((BT_HDR *)data, NULL, NULL, NULL);
    ALOGW("%s legacy transmit of command. Use transmit_command instead.", __func__);
  } else {
    fixed_queue_enqueue(packet_queue, data);
  }
}

// Internal functions

static void command_timed_out(UNUSED_ATTR void *context) {
  pthread_mutex_lock(&commands_pending_response_lock);

  if (list_is_empty(commands_pending_response)) {
    ALOGE("%s with no commands pending response", __func__);
    return;
  }

  waiting_command_t *wait_entry = list_front(commands_pending_response);
  pthread_mutex_unlock(&commands_pending_response_lock);

  // We shouldn't try to recover the stack from this command timeout.
  // If it's caused by a software bug, fix it. If it's a hardware bug, fix it.
  ALOGE("%s hci layer timeout waiting for response to a command. opcode: 0x%x", __func__, wait_entry->opcode);
  ALOGE("%s restarting the bluetooth process.", __func__);
  usleep(10000);
  kill(getpid(), SIGKILL);
}

static void restart_command_timeout_alarm() {
  if (list_is_empty(commands_pending_response))
    alarm_cancel(command_response_alarm);
  else
    alarm_set(command_response_alarm, COMMAND_PENDING_TIMEOUT, command_timed_out, NULL);
}

static waiting_command_t *get_waiting_command(command_opcode_t opcode) {
  pthread_mutex_lock(&commands_pending_response_lock);

  for (const list_node_t *node = list_begin(commands_pending_response);
      node != list_end(commands_pending_response);
      node = list_next(node)) {
    waiting_command_t *wait_entry = list_node(node);

    if (!wait_entry || wait_entry->opcode != opcode)
      continue;

    list_remove(commands_pending_response, wait_entry);

    pthread_mutex_unlock(&commands_pending_response_lock);
    return wait_entry;
  }

  pthread_mutex_unlock(&commands_pending_response_lock);
  return NULL;
}

// Inspects an incoming event for interesting information, like how many
// commands are now able to be sent. Returns true if the event should
// not proceed to higher layers (i.e. was intercepted).
static bool filter_incoming_event(BT_HDR *packet) {
  waiting_command_t *wait_entry = NULL;
  uint8_t *stream = packet->data;
  uint8_t event_code;
  command_opcode_t opcode;

  STREAM_TO_UINT8(event_code, stream);
  STREAM_SKIP_UINT8(stream); // Skip the parameter total length field

  if (event_code == HCI_COMMAND_COMPLETE_EVT) {
    STREAM_TO_UINT8(command_credits, stream);
    STREAM_TO_UINT16(opcode, stream);

    wait_entry = get_waiting_command(opcode);
    if (!wait_entry)
      ALOGW("%s command complete event with no matching command. opcode: 0x%x.", __func__, opcode);
    else if (wait_entry->complete_callback)
      wait_entry->complete_callback(packet, wait_entry->context);

    goto intercepted;
  } else if (event_code == HCI_COMMAND_STATUS_EVT) {
    uint8_t status;
    STREAM_TO_UINT8(status, stream);
    STREAM_TO_UINT8(command_credits, stream);
    STREAM_TO_UINT16(opcode, stream);

    // If a command generates a command status event, it won't be getting a command complete event

    wait_entry = get_waiting_command(opcode);
    if (!wait_entry)
      ALOGW("%s command status event with no matching command. opcode: 0x%x", __func__, opcode);
    else if (wait_entry->status_callback)
      wait_entry->status_callback(status, wait_entry->command, wait_entry->context);

    goto intercepted;
  }

  return false;
intercepted:;
  restart_command_timeout_alarm();
  if (wait_entry) {
    // If it has a callback, it's responsible for freeing the packet
    if (event_code == HCI_COMMAND_STATUS_EVT || !wait_entry->complete_callback)
      buffer_allocator->free(packet);

    // If it has a callback, it's responsible for freeing the command
    if (event_code == HCI_COMMAND_COMPLETE_EVT || !wait_entry->status_callback)
      buffer_allocator->free(wait_entry->command);

    osi_free(wait_entry);
  } else {
    buffer_allocator->free(packet);
  }

  return true;
}

static void on_controller_acl_size_fetch_finished(void) {
  ALOGI("%s postload finished.", __func__);
}

static void sco_config_callback(UNUSED_ATTR bool success) {
  controller->begin_acl_size_fetch(on_controller_acl_size_fetch_finished);
}

static void firmware_config_callback(UNUSED_ATTR bool success) {
  firmware_is_configured = true;
  callbacks->preload_finished(true);
}

static void epilog_finished_callback(UNUSED_ATTR bool success) {
  ALOGI("%s", __func__);
  thread_stop(thread);
}

static void epilog_wait_timer_expired(UNUSED_ATTR void *context) {
  ALOGI("%s", __func__);
  thread_stop(thread);
}

static void event_preload(UNUSED_ATTR void *context) {
  ALOGI("%s", __func__);
  hal->open();
  vendor->send_async_command(VENDOR_CONFIGURE_FIRMWARE, NULL);
}

static void event_postload(UNUSED_ATTR void *context) {
  ALOGI("%s", __func__);
  if(vendor->send_async_command(VENDOR_CONFIGURE_SCO, NULL) == -1) {
    // If couldn't configure sco, we won't get the sco configuration callback
    // so go pretend to do it now
    sco_config_callback(false);
  }
}

static void event_epilog(UNUSED_ATTR void *context) {
  vendor->send_async_command(VENDOR_DO_EPILOG, NULL);
}

static void event_command_ready(fixed_queue_t *queue, UNUSED_ATTR void *context) {
  if (command_credits > 0) {
    waiting_command_t *wait_entry = fixed_queue_dequeue(queue);
    command_credits--;

    // Move it to the list of commands awaiting response
    pthread_mutex_lock(&commands_pending_response_lock);
    list_append(commands_pending_response, wait_entry);
    pthread_mutex_unlock(&commands_pending_response_lock);

    // Send it off
    low_power_manager->wake_assert();
    packet_fragmenter->fragment_and_dispatch(wait_entry->command);
    low_power_manager->transmit_done();

    restart_command_timeout_alarm();
  }
}

static void event_packet_ready(fixed_queue_t *queue, UNUSED_ATTR void *context) {
  // The queue may be the command queue or the packet queue, we don't care
  BT_HDR *packet = (BT_HDR *)fixed_queue_dequeue(queue);

  low_power_manager->wake_assert();
  packet_fragmenter->fragment_and_dispatch(packet);
  low_power_manager->transmit_done();
}

// This function is not required to read all of a packet in one go, so
// be wary of reentry. But this function must return after finishing a packet.
static void hal_says_data_ready(serial_data_type_t type) {
  packet_receive_data_t *incoming = &incoming_packets[PACKET_TYPE_TO_INBOUND_INDEX(type)];

  uint8_t byte;
  while (hal->read_data(type, &byte, 1, false) != 0) {
    switch (incoming->state) {
      case BRAND_NEW:
        // Initialize and prepare to jump to the preamble reading state
        incoming->bytes_remaining = preamble_sizes[PACKET_TYPE_TO_INDEX(type)];
        memset(incoming->preamble, 0, PREAMBLE_BUFFER_SIZE);
        incoming->index = 0;
        incoming->state = PREAMBLE;
        // INTENTIONAL FALLTHROUGH
      case PREAMBLE:
        incoming->preamble[incoming->index] = byte;
        incoming->index++;
        incoming->bytes_remaining--;

        if (incoming->bytes_remaining == 0) {
          // For event and sco preambles, the last byte we read is the length
          incoming->bytes_remaining = (type == DATA_TYPE_ACL) ? RETRIEVE_ACL_LENGTH(incoming->preamble) : byte;

          size_t buffer_size = BT_HDR_SIZE + incoming->index + incoming->bytes_remaining;
          incoming->buffer = (BT_HDR *)buffer_allocator->alloc(buffer_size);

          if (!incoming->buffer) {
            ALOGE("%s error getting buffer for incoming packet", __func__);
            // Can't read any more of this current packet, so jump out
            incoming->state = incoming->bytes_remaining == 0 ? BRAND_NEW : IGNORE;
            break;
          }

          // Initialize the buffer
          incoming->buffer->offset = 0;
          incoming->buffer->layer_specific = 0;
          incoming->buffer->event = outbound_event_types[PACKET_TYPE_TO_INDEX(type)];
          memcpy(incoming->buffer->data, incoming->preamble, incoming->index);

          incoming->state = incoming->bytes_remaining > 0 ? BODY : FINISHED;
        }

        break;
      case BODY:
        incoming->buffer->data[incoming->index] = byte;
        incoming->index++;
        incoming->bytes_remaining--;

        size_t bytes_read = hal->read_data(type, (incoming->buffer->data + incoming->index), incoming->bytes_remaining, false);
        incoming->index += bytes_read;
        incoming->bytes_remaining -= bytes_read;

        incoming->state = incoming->bytes_remaining == 0 ? FINISHED : incoming->state;
        break;
      case IGNORE:
        incoming->bytes_remaining--;
        incoming->state = incoming->bytes_remaining == 0 ? BRAND_NEW : incoming->state;
        break;
      case FINISHED:
        ALOGE("%s the state machine should not have been left in the finished state.", __func__);
        break;
    }

    if (incoming->state == FINISHED) {
      incoming->buffer->len = incoming->index;
      btsnoop->capture(incoming->buffer, true);

      if (type != DATA_TYPE_EVENT || !filter_incoming_event(incoming->buffer)) {
        packet_fragmenter->reassemble_and_dispatch(incoming->buffer);
      }

      // We don't control the buffer anymore
      incoming->buffer = NULL;
      incoming->state = BRAND_NEW;
      hal->packet_finished(type);

      // We return after a packet is finished for two reasons:
      // 1. The type of the next packet could be different.
      // 2. We don't want to hog cpu time.
      return;
    }
  }
}

// TODO(zachoverflow): we seem to do this a couple places, like the HCI inject module. #centralize
static serial_data_type_t event_to_data_type(uint16_t event) {
  if (event == MSG_STACK_TO_HC_HCI_ACL)
    return DATA_TYPE_ACL;
  else if (event == MSG_STACK_TO_HC_HCI_SCO)
    return DATA_TYPE_SCO;
  else if (event == MSG_STACK_TO_HC_HCI_CMD)
    return DATA_TYPE_COMMAND;
  else
    ALOGE("%s invalid event type, could not translate 0x%x", __func__, event);

  return 0;
}

// Callback for the fragmenter to send a fragment
static void transmit_fragment(BT_HDR *packet, bool send_transmit_finished) {
  uint16_t event = packet->event & MSG_EVT_MASK;
  serial_data_type_t type = event_to_data_type(event);

  btsnoop->capture(packet, false);
  hal->transmit_data(type, packet->data + packet->offset, packet->len);

  if (event != MSG_STACK_TO_HC_HCI_CMD && send_transmit_finished)
    callbacks->transmit_finished(packet, true);
}

// Callback for the fragmenter to dispatch up a completely reassembled packet
static void dispatch_reassembled(BT_HDR *packet) {
  data_dispatcher_dispatch(
    interface.upward_dispatcher,
    packet->event & MSG_EVT_MASK,
    packet
  );
}

static void fragmenter_transmit_finished(void *buffer, bool all_fragments_sent) {
  callbacks->transmit_finished(buffer, all_fragments_sent);
}

static void init_layer_interface() {
  if (!interface_created) {
    interface.start_up = start_up;
    interface.shut_down = shut_down;

    interface.set_chip_power_on = set_chip_power_on;
    interface.send_low_power_command = low_power_manager->post_command;
    interface.do_preload = do_preload;
    interface.do_postload = do_postload;
    interface.turn_on_logging = turn_on_logging;
    interface.turn_off_logging = turn_off_logging;

    // It's probably ok for this to live forever. It's small and
    // there's only one instance of the hci interface.
    interface.upward_dispatcher = data_dispatcher_new("hci_layer");
    if (!interface.upward_dispatcher) {
      ALOGE("%s could not create upward dispatcher.", __func__);
      return;
    }

    interface.transmit_command = transmit_command;
    interface.transmit_downward = transmit_downward;
    interface_created = true;
  }
}

static const hci_hal_callbacks_t hal_callbacks = {
  hal_says_data_ready
};

static const packet_fragmenter_callbacks_t packet_fragmenter_callbacks = {
  transmit_fragment,
  dispatch_reassembled,
  fragmenter_transmit_finished
};

const hci_t *hci_layer_get_interface() {
  buffer_allocator = buffer_allocator_get_interface();
  hal = hci_hal_get_interface();
  btsnoop = btsnoop_get_interface();
  controller = controller_get_interface();
  hci_inject = hci_inject_get_interface();
  packet_fragmenter = packet_fragmenter_get_interface();
  vendor = vendor_get_interface();
  low_power_manager = low_power_manager_get_interface();

  init_layer_interface();
  return &interface;
}

const hci_t *hci_layer_get_test_interface(
    const allocator_t *buffer_allocator_interface,
    const hci_hal_t *hal_interface,
    const btsnoop_t *btsnoop_interface,
    const controller_t *controller_interface,
    const hci_inject_t *hci_inject_interface,
    const packet_fragmenter_t *packet_fragmenter_interface,
    const vendor_t *vendor_interface,
    const low_power_manager_t *low_power_manager_interface) {

  buffer_allocator = buffer_allocator_interface;
  hal = hal_interface;
  btsnoop = btsnoop_interface;
  controller = controller_interface;
  hci_inject = hci_inject_interface;
  packet_fragmenter = packet_fragmenter_interface;
  vendor = vendor_interface;
  low_power_manager = low_power_manager_interface;

  init_layer_interface();
  return &interface;
}
