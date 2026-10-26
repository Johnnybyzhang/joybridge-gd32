/* Compile the unmodified HCI implementation into this fixture so only radio
 * boot can be bypassed. BTstack's upstream fuzz helpers assume BLE is enabled;
 * this build deliberately retains the production Classic-only configuration. */
#include "../third_party/btstack/src/hci.c"
hci_stack_t *test_hci_working(void) {
    hci_stack->hci_packet_buffer_reserved=false;
    hci_stack->acl_data_packet_length=1021;
    hci_stack->acl_packets_total_num=8;
    hci_stack->num_cmd_packets=1;
    hci_init_done();
    return hci_stack;
}
