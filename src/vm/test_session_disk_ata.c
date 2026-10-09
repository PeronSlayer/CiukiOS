/* Host regression for the bounded legacy ATA backend. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "session_disk_ata.c"

#define PRIMARY_CMD 0x1f0U
#define PRIMARY_CTL 0x3f6U
#define TEST_SECTORS 8U
#define ATA_ST_DRDY 0x40U
#define STATUS_TRACE_CAPACITY 4096U
#define TRACE_REGULAR_STATUS 1U
#define TRACE_ALTERNATE_STATUS 2U

static uint8_t disk[TEST_SECTORS][512];
static uint8_t identify_data[512];
static uint8_t task[8];
static uint8_t ata_status_value;
static uint8_t ata_error_value;
static uint8_t device_control_value;
static uint8_t selected_device_head, target_device_head;
static uint8_t pic_irr, pic_isr, pic_select;
static uint32_t command_count, device_control_writes, ata_io_count;
static uint32_t regular_status_reads, alternate_status_reads;
static uint8_t status_trace[STATUS_TRACE_CAPACITY];
static uint32_t status_trace_count;
static uint32_t last_status_trace_kind;
static uint32_t bulk_read_calls, bulk_write_calls;
static uint32_t bulk_read_words, bulk_write_words;
static uint32_t direct_data_read_words, direct_data_write_words;
static uint32_t in_bulk_read, in_bulk_write, bad_bulk_port_count;
static uint32_t fake_clock_frequency, fake_clock_samples;
static uint32_t fake_clock_last_high, fake_clock_high_transitions;
static uint64_t fake_clock_ticks, fake_clock_step;
static uint32_t fake_clock_full_wraps;
static uint32_t command_status_delay_config, command_status_delay_remaining;
static uint32_t long_poll_status_samples, long_poll_clock_samples;
static uint32_t timeout_clock_samples, frozen_clock_samples, rollover_observed;
static uint32_t full_wrap_observed, deadline_product_cases;
static uint32_t intrq_ack_count, intrq_pending, bad_control_restore_count;
static uint8_t control_write_log[128];
static uint32_t control_write_log_count, command_issue_serial;
static uint32_t command_regular_ack, command_idle_seen;
static uint32_t status_mode, command_stuck_mode, fail_command;
static uint32_t old_selected_device_absent;
static uint32_t current_command, current_lba, data_word_index;
static uint32_t checks;

enum { MODE_NORMAL = 0, MODE_STUCK_BUSY = 1, MODE_STUCK_DRQ = 2 };

static void check(int condition, const char *expr, int line)
{
    ++checks;
    if (!condition) {
        fprintf(stderr, "check %u failed at line %d: %s\n", checks, line, expr);
        exit(1);
    }
}

#define CHECK(expr) check((expr) != 0, #expr, __LINE__)

static uint8_t *active_data(void)
{
    if (current_command == ATA_IDENTIFY) return identify_data;
    if (current_lba >= TEST_SECTORS) return disk[0];
    return disk[current_lba];
}

uint32_t cvata_clock_khz(void)
{
    return fake_clock_frequency;
}

void cvclock_now(uint32_t *low, uint32_t *high)
{
    uint32_t current_high;
    uint64_t previous = fake_clock_ticks;
    ++fake_clock_samples;
    fake_clock_ticks += fake_clock_step;
    if (fake_clock_ticks < previous) ++fake_clock_full_wraps;
    current_high = (uint32_t)(fake_clock_ticks >> 32);
    if (current_high != fake_clock_last_high) {
        ++fake_clock_high_transitions;
        fake_clock_last_high = current_high;
    }
    *low = (uint32_t)fake_clock_ticks;
    *high = current_high;
}

uint32_t cvdev_in(uint32_t port, uint32_t size)
{
    if (port == PIC2_COMMAND && size == 1U)
        return pic_select == PIC_OCW3_ISR ? pic_isr : pic_irr;
    if (port >= PRIMARY_CMD && port < PRIMARY_CMD + 8U) {
        uint32_t reg = port - PRIMARY_CMD;
        ++ata_io_count;
        if (size == 1U && reg == ATA_STATUS) {
            ++regular_status_reads;
            last_status_trace_kind = TRACE_REGULAR_STATUS;
            if (status_trace_count < STATUS_TRACE_CAPACITY)
                status_trace[status_trace_count++] = TRACE_REGULAR_STATUS;
            if (intrq_pending) {
                ++intrq_ack_count;
                intrq_pending = 0U;
                command_regular_ack = command_issue_serial;
            }
            return ata_status_value;
        }
        if (size == 1U && reg == ATA_ERROR) return ata_error_value;
        if (size == 2U && reg == ATA_DATA) {
            if (!in_bulk_read) ++direct_data_read_words;
            uint8_t *data = active_data();
            uint32_t offset = data_word_index * 2U;
            uint16_t word = (uint16_t)data[offset] |
                            ((uint16_t)data[offset + 1U] << 8);
            ++data_word_index;
            if (data_word_index == 256U) ata_status_value = ATA_ST_DRDY;
            return word;
        }
    }
    if (port == PRIMARY_CTL && size == 1U) {
        ++ata_io_count;
        ++alternate_status_reads;
        last_status_trace_kind = TRACE_ALTERNATE_STATUS;
        if (status_trace_count < STATUS_TRACE_CAPACITY)
            status_trace[status_trace_count++] = TRACE_ALTERNATE_STATUS;
        if (status_mode == MODE_STUCK_BUSY) return ATA_ST_BSY;
        if (status_mode == MODE_STUCK_DRQ) return ATA_ST_DRQ;
        if (command_status_delay_remaining != 0U) {
            --command_status_delay_remaining;
            return ATA_ST_BSY;
        }
        if (old_selected_device_absent && selected_device_head != target_device_head)
            return 0U;
        if (command_issue_serial != 0U &&
            (ata_status_value & (ATA_ST_BSY | ATA_ST_DRQ)) == 0U)
            command_idle_seen = command_issue_serial;
        return ata_status_value;
    }
    return 0U;
}

void cvdev_out(uint32_t port, uint32_t value, uint32_t size)
{
    if (port == PIC2_COMMAND && size == 1U) {
        pic_select = (uint8_t)value;
        return;
    }
    if (port == PRIMARY_CTL && size == 1U) {
        ++device_control_writes;
        device_control_value = (uint8_t)value;
        if (control_write_log_count < sizeof control_write_log)
            control_write_log[control_write_log_count++] = (uint8_t)value;
        /* A command must be observed idle and have INTRQ acknowledged before
         * the saved firmware control value is restored. */
        if ((value & ATA_CTL_NIEN) == 0U && command_issue_serial != 0U &&
            (command_idle_seen != command_issue_serial ||
             command_regular_ack != command_issue_serial))
            ++bad_control_restore_count;
        return;
    }
    if (port < PRIMARY_CMD || port >= PRIMARY_CMD + 8U) return;
    ++ata_io_count;
    if (size == 1U && port - PRIMARY_CMD == ATA_COMMAND) {
        ++command_count;
        ++command_issue_serial;
        command_regular_ack = 0U;
        command_idle_seen = 0U;
        intrq_pending = 1U;
        current_command = (uint8_t)value;
        current_lba = (uint32_t)task[ATA_LBA_LOW] |
                      ((uint32_t)task[ATA_LBA_MID] << 8) |
                      ((uint32_t)task[ATA_LBA_HIGH] << 16) |
                      (((uint32_t)task[ATA_DEVICE] & 0x0fU) << 24);
        data_word_index = 0U;
        command_status_delay_remaining = command_status_delay_config;
        if (fail_command == current_command) {
            ata_status_value = ATA_ST_DRDY | ATA_ST_ERR;
            ata_error_value = 0x04U;
        } else if (command_stuck_mode == MODE_STUCK_BUSY) {
            status_mode = MODE_STUCK_BUSY;
            ata_status_value = ATA_ST_BSY;
            intrq_pending = 0U; /* no terminal device event while BSY persists */
        } else if (command_stuck_mode == MODE_STUCK_DRQ) {
            status_mode = MODE_STUCK_DRQ;
            ata_status_value = ATA_ST_DRQ;
        } else {
            ata_status_value = ATA_ST_DRDY | ATA_ST_DRQ;
            if (current_command == ATA_WRITE) {
                if (current_lba < TEST_SECTORS) memset(disk[current_lba], 0, 512U);
            }
        }
        return;
    }
    if (size == 1U) {
        uint32_t reg = port - PRIMARY_CMD;
        if (reg < 8U) {
            task[reg] = (uint8_t)value;
            if (reg == ATA_DEVICE) {
                selected_device_head = (uint8_t)value & 0xf0U;
                if (selected_device_head == target_device_head)
                    ata_status_value = ATA_ST_DRDY;
            }
        }
        return;
    }
    if (size == 2U && port - PRIMARY_CMD == ATA_DATA && current_command == ATA_WRITE) {
        if (!in_bulk_write) ++direct_data_write_words;
        uint8_t *data = active_data();
        uint32_t offset = data_word_index * 2U;
        data[offset] = (uint8_t)value;
        data[offset + 1U] = (uint8_t)(value >> 8);
        ++data_word_index;
        if (data_word_index == 256U) ata_status_value = ATA_ST_DRDY;
    }
}

void cvata_pio_read(uint32_t port, uint8_t *buffer)
{
    uint32_t i;
    ++bulk_read_calls;
    if (port != PRIMARY_CMD + ATA_DATA) ++bad_bulk_port_count;
    in_bulk_read = 1U;
    for (i = 0U; i < 256U; ++i) {
        uint16_t value = (uint16_t)cvdev_in(port, 2U);
        buffer[i * 2U] = (uint8_t)value;
        buffer[i * 2U + 1U] = (uint8_t)(value >> 8);
        ++bulk_read_words;
    }
    in_bulk_read = 0U;
}

void cvata_pio_write(uint32_t port, const uint8_t *buffer)
{
    uint32_t i;
    ++bulk_write_calls;
    if (port != PRIMARY_CMD + ATA_DATA) ++bad_bulk_port_count;
    in_bulk_write = 1U;
    for (i = 0U; i < 256U; ++i) {
        uint16_t value = (uint16_t)buffer[i * 2U] |
                         ((uint16_t)buffer[i * 2U + 1U] << 8);
        cvdev_out(port, value, 2U);
        ++bulk_write_words;
    }
    in_bulk_write = 0U;
}

static void reset_fake(void)
{
    uint32_t i;
    memset(&g_status, 0, sizeof g_status);
    memset(&g_discovery, 0, sizeof g_discovery);
    memset(g_identify, 0, sizeof g_identify);
    g_control_saved = g_control_owned = g_command_issued = 0U;
    memset(disk, 0, sizeof disk);
    memset(identify_data, 0, sizeof identify_data);
    memset(task, 0, sizeof task);
    ata_status_value = ATA_ST_DRDY;
    ata_error_value = 0U;
    device_control_value = 0U;
    selected_device_head = target_device_head = 0xe0U;
    old_selected_device_absent = 0U;
    pic_irr = pic_isr = pic_select = 0U;
    command_count = device_control_writes = ata_io_count = 0U;
    regular_status_reads = alternate_status_reads = 0U;
    memset(status_trace, 0, sizeof status_trace);
    status_trace_count = 0U;
    last_status_trace_kind = 0U;
    bulk_read_calls = bulk_write_calls = 0U;
    bulk_read_words = bulk_write_words = 0U;
    direct_data_read_words = direct_data_write_words = 0U;
    in_bulk_read = in_bulk_write = bad_bulk_port_count = 0U;
    fake_clock_frequency = 1000U;
    fake_clock_samples = fake_clock_last_high = fake_clock_high_transitions = 0U;
    fake_clock_ticks = 0U;
    fake_clock_step = 1000U; /* 1 ms at 1000 cycles per millisecond */
    fake_clock_full_wraps = 0U;
    command_status_delay_config = command_status_delay_remaining = 0U;
    intrq_ack_count = intrq_pending = bad_control_restore_count = 0U;
    memset(control_write_log, 0, sizeof control_write_log);
    control_write_log_count = command_issue_serial = 0U;
    command_regular_ack = command_idle_seen = 0U;
    status_mode = command_stuck_mode = fail_command = current_command = current_lba = data_word_index = 0U;
    g_clock_khz = 0U;

    /* ATA IDENTIFY: ATA disk, LBA supported, 32-bit sector capacity, 512 B sectors. */
    identify_data[0] = 0x40U;
    identify_data[49U * 2U] = 0x00U;
    identify_data[49U * 2U + 1U] = 0x02U;
    identify_data[60U * 2U] = TEST_SECTORS;
    identify_data[106U * 2U] = 0x00U;
    identify_data[106U * 2U + 1U] = 0x40U;
    for (i = 0U; i < TEST_SECTORS; ++i) {
        uint32_t j;
        for (j = 0U; j < 512U; ++j)
            disk[i][j] = (uint8_t)(i * 29U + j * 7U + (j >> 2));
    }
}

static void make_packet(uint8_t packet[64])
{
    uint32_t i, sum = 0U;
    memset(packet, 0, 64U);
    packet[0] = 'A'; packet[1] = 'D'; packet[2] = 'P'; packet[3] = '1';
    packet[4] = 64U;
    packet[6] = 0x80U;
    packet[8] = 30U;
    packet[8 + 16] = TEST_SECTORS;
    packet[8 + 24] = 0x00U;
    packet[8 + 25] = 0x02U;
    packet[8 + 26] = 0U;
    packet[8 + 27] = 0U;
    packet[8 + 28] = 0U;
    packet[8 + 29] = 0U;
    packet[40] = 0xf0U; packet[41] = 0x01U;
    packet[42] = 0xf6U; packet[43] = 0x03U;
    packet[44] = 0xe0U;
    packet[46] = 14U;
    packet[50] = 0x10U;
    packet[54] = 0x11U;
    packet[56] = 16U; packet[58] = 63U; packet[60] = 1024U & 0xffU;
    packet[61] = 1024U >> 8;
    for (i = 40U; i < 56U; ++i) sum += packet[i];
    packet[55] = (uint8_t)(0U - (sum & 0xffU));
}

static void refresh_checksum(uint8_t packet[64])
{
    uint32_t i, sum = 0U;
    packet[55] = 0U;
    for (i = 40U; i < 56U; ++i) sum += packet[i];
    packet[55] = (uint8_t)(0U - (sum & 0xffU));
}

static void prepare_bound_with_control(uint8_t bios_lba0[512], uint8_t control)
{
    uint8_t packet[64];
    make_packet(packet);
    memcpy(bios_lba0, disk[0], 512U);
    CHECK(cvata_prepare(packet) == CVATA_OK);
    CHECK(command_count == 0U);
    device_control_value = control;
    cvata_observe_control(control, 1U);
    CHECK(cvata_bind(bios_lba0) == CVATA_OK);
    CHECK(cvata_get_status()->state == CVATA_STATE_BOUND);
    CHECK(cvata_get_status()->ready == 1U);
}

static void prepare_bound(uint8_t bios_lba0[512])
{
    prepare_bound_with_control(bios_lba0, 0U);
}

static void check_control_restored(uint8_t control)
{
    CHECK(device_control_value == control);
    CHECK(bad_control_restore_count == 0U);
    CHECK(intrq_pending == 0U);
    CHECK(regular_status_reads > 0U);
    CHECK(alternate_status_reads > 0U);
    CHECK(status_trace_count > 0U);
    CHECK(status_trace[0] == TRACE_ALTERNATE_STATUS);
    CHECK(last_status_trace_kind == TRACE_REGULAR_STATUS);
}

static void test_validation_has_no_ata_commands(void)
{
    uint8_t packet[64], bios[512];
    const uint8_t unknown_dpte_revisions[] = {0x00U, 0x01U, 0x0fU, 0x12U, 0x20U, 0x30U, 0xffU};
    uint32_t i;
    reset_fake();
    make_packet(packet);
    packet[55] ^= 1U;
    CHECK(cvata_prepare(packet) == CVATA_E_PACKET);
    CHECK(command_count == 0U && ata_io_count == 0U);

    reset_fake();
    make_packet(packet); packet[40] = 0x00U; packet[41] = 0x02U;
    refresh_checksum(packet);
    CHECK(cvata_prepare(packet) == CVATA_E_UNSUPPORTED);
    CHECK(command_count == 0U && ata_io_count == 0U);

    reset_fake();
    make_packet(packet); packet[8 + 20] = 1U; /* capacity above 32 bits */
    CHECK(cvata_prepare(packet) == CVATA_E_UNSUPPORTED);
    CHECK(command_count == 0U && ata_io_count == 0U);

    reset_fake();
    make_packet(packet);
    memcpy(bios, disk[0], 512U);
    CHECK(cvata_prepare(packet) == CVATA_OK);
    CHECK(cvata_bind(bios) == CVATA_E_UNSUPPORTED); /* no control shadow */
    CHECK(command_count == 0U && device_control_writes == 0U);

    for (i = 0U; i < sizeof unknown_dpte_revisions; ++i) {
        reset_fake();
        make_packet(packet);
        packet[54] = unknown_dpte_revisions[i];
        refresh_checksum(packet); /* valid DPTE checksum, unknown structure revision */
        CHECK(cvata_prepare(packet) == CVATA_E_UNSUPPORTED);
        CHECK(command_count == 0U && ata_io_count == 0U);
    }

    reset_fake();
    make_packet(packet); packet[40] = 0x68U; packet[41] = 0x01U; /* invalid DPTE command base */
    refresh_checksum(packet);
    CHECK(cvata_prepare(packet) == CVATA_E_UNSUPPORTED);
    CHECK(command_count == 0U && ata_io_count == 0U);
}

static void test_busy_pic_has_no_command(void)
{
    uint8_t bios[512], packet[64];
    reset_fake();
    make_packet(packet);
    memcpy(bios, disk[0], 512U);
    CHECK(cvata_prepare(packet) == CVATA_OK);
    cvata_observe_control(0U, 1U);
    pic_irr = 0x40U;
    CHECK(cvata_bind(bios) == CVATA_E_BUSY);
    CHECK(command_count == 0U);
    CHECK(pic_select == PIC_OCW3_IRR);
    CHECK(device_control_writes == 0U);
}

static void test_bind_and_sector_io(void)
{
    uint8_t bios[512], buffer_storage[514], original[512], changed[512];
    uint8_t *buffer = buffer_storage + 1U;
    const struct cvata_status *s;
    uint32_t i;
    reset_fake();
    prepare_bound(bios);
    CHECK(command_count == 2U); /* IDENTIFY then native READ LBA 0 */
    CHECK(bulk_read_calls == 2U && bulk_read_words == 512U);
    CHECK(bulk_write_calls == 0U && bulk_write_words == 0U);
    CHECK(direct_data_read_words == 0U && direct_data_write_words == 0U);
    CHECK(bad_bulk_port_count == 0U);
    buffer_storage[0] = 0x5aU;
    buffer_storage[513] = 0xa5U;
    check_control_restored(0U);
    CHECK(memcmp(bios, disk[0], 512U) == 0);
    memcpy(original, disk[1], 512U);
    memset(buffer, 0, 512U);
    CHECK(cvata_transfer(0U, 1U, buffer) == CVATA_OK);
    CHECK(memcmp(buffer, original, 512U) == 0);
    CHECK(bulk_read_calls == 3U && bulk_read_words == 768U);
    CHECK(buffer_storage[0] == 0x5aU && buffer_storage[513] == 0xa5U);
    check_control_restored(0U);
    for (i = 0U; i < 512U; ++i) changed[i] = (uint8_t)(255U - i);
    CHECK(cvata_transfer(1U, 1U, changed) == CVATA_OK);
    CHECK(bulk_write_calls == 1U && bulk_write_words == 256U);
    CHECK(buffer_storage[0] == 0x5aU && buffer_storage[513] == 0xa5U);
    memset(buffer, 0, 512U);
    CHECK(cvata_transfer(0U, 1U, buffer) == CVATA_OK);
    CHECK(memcmp(buffer, changed, 512U) == 0);
    CHECK(bulk_read_calls == 4U && bulk_read_words == 1024U);
    CHECK(bulk_write_calls == 1U && bulk_write_words == 256U);
    CHECK(direct_data_read_words == 0U && direct_data_write_words == 0U);
    CHECK(bad_bulk_port_count == 0U);
    CHECK(buffer_storage[0] == 0x5aU && buffer_storage[513] == 0xa5U);
    check_control_restored(0U);
    s = cvata_get_status();
    CHECK(s->read_count == 2U && s->write_count == 1U);
    CHECK(s->last_lba == 1U && s->capacity_sectors == TEST_SECTORS);
    CHECK(s->logical_heads == 16U && s->sectors_per_track == 63U &&
          s->cylinders == 1024U);
    CHECK(s->control_shadow == 0U);
    CHECK(cvata_transfer(0U, TEST_SECTORS, buffer) == CVATA_E_RANGE);
}

static void test_firmware_control_preserved_and_error_cleanup(void)
{
    uint8_t bios[512], packet[64];
    uint32_t before;

    reset_fake();
    make_packet(packet);
    memcpy(bios, disk[0], 512U);
    CHECK(cvata_prepare(packet) == CVATA_OK);
    device_control_value = 0x01U; /* preserve an unrelated observed control bit */
    cvata_observe_control(0x01U, 1U);
    CHECK(cvata_bind(bios) == CVATA_OK);
    CHECK(device_control_value == 0x01U);
    CHECK(control_write_log_count >= 2U && (control_write_log_count & 1U) == 0U);
    CHECK(control_write_log[0] == 0x03U);
    CHECK(control_write_log[control_write_log_count - 1U] == 0x01U);
    {
        uint32_t i;
        for (i = 0U; i < control_write_log_count; i += 2U) {
            CHECK(control_write_log[i] == 0x03U);
            CHECK(control_write_log[i + 1U] == 0x01U);
        }
    }
    CHECK(bad_control_restore_count == 0U && intrq_pending == 0U);

    reset_fake();
    make_packet(packet);
    memcpy(bios, disk[0], 512U);
    CHECK(cvata_prepare(packet) == CVATA_OK);
    device_control_value = 0x01U;
    cvata_observe_control(0x01U, 1U);
    fail_command = ATA_IDENTIFY;
    CHECK(cvata_bind(bios) == CVATA_E_ATA);
    check_control_restored(0x01U);

    reset_fake();
    make_packet(packet);
    memcpy(bios, disk[0], 512U);
    CHECK(cvata_prepare(packet) == CVATA_OK);
    device_control_value = 0x01U;
    cvata_observe_control(0x01U, 1U);
    bios[127] ^= 0x80U;
    CHECK(cvata_bind(bios) == CVATA_E_UNSUPPORTED); /* LBA0 compare mismatch */
    check_control_restored(0x01U);

    reset_fake();
    make_packet(packet);
    memcpy(bios, disk[0], 512U);
    CHECK(cvata_prepare(packet) == CVATA_OK);
    device_control_value = 0x02U; /* firmware already had nIEN set */
    cvata_observe_control(0x02U, 1U);
    CHECK(cvata_bind(bios) == CVATA_OK);
    CHECK(device_control_value == 0x02U);
    before = control_write_log_count;
    CHECK((before & 1U) == 0U);
    CHECK(before == 0U || control_write_log[before - 1U] == 0x02U);
}

static void test_stale_error_and_preexisting_busy(void)
{
    uint8_t bios[512], packet[64];

    reset_fake();
    make_packet(packet);
    memcpy(bios, disk[0], 512U);
    CHECK(cvata_prepare(packet) == CVATA_OK);
    cvata_observe_control(0U, 1U);
    ata_status_value = ATA_ST_DRDY | ATA_ST_ERR;
    CHECK(cvata_bind(bios) == CVATA_OK); /* stale ERR is cleared by issuing IDENTIFY */
    check_control_restored(0U);

    reset_fake();
    make_packet(packet);
    memcpy(bios, disk[0], 512U);
    CHECK(cvata_prepare(packet) == CVATA_OK);
    cvata_observe_control(0U, 1U);
    ata_status_value = ATA_ST_BSY;
    CHECK(cvata_bind(bios) == CVATA_E_BUSY);
    CHECK(command_count == 0U && device_control_writes == 0U);

    reset_fake();
    make_packet(packet);
    memcpy(bios, disk[0], 512U);
    CHECK(cvata_prepare(packet) == CVATA_OK);
    cvata_observe_control(0U, 1U);
    ata_status_value = ATA_ST_DRQ;
    CHECK(cvata_bind(bios) == CVATA_E_BUSY);
    CHECK(command_count == 0U && device_control_writes == 0U);
}

static void test_empty_previous_selection_before_target_select(void)
{
    uint8_t bios[512], packet[64];
    reset_fake();
    make_packet(packet);
    packet[44] = 0xf0U; /* DPTE selects primary slave */
    refresh_checksum(packet);
    memcpy(bios, disk[0], 512U);
    CHECK(cvata_prepare(packet) == CVATA_OK);
    cvata_observe_control(0U, 1U);
    selected_device_head = 0xe0U;
    target_device_head = 0xf0U;
    old_selected_device_absent = 1U;
    CHECK(cvata_bind(bios) == CVATA_OK);
    CHECK(selected_device_head == target_device_head);
    CHECK(command_count == 2U);
    check_control_restored(0U);
}

static void test_identify_capacity_and_sector_size(void)
{
    uint8_t bios[512], packet[64];

    reset_fake();
    make_packet(packet);
    memcpy(bios, disk[0], 512U);
    CHECK(cvata_prepare(packet) == CVATA_OK);
    cvata_observe_control(0U, 1U);
    identify_data[60U * 2U] = (uint8_t)(TEST_SECTORS - 1U);
    CHECK(cvata_bind(bios) == CVATA_E_UNSUPPORTED); /* DPTE/IDENTIFY capacity mismatch */
    check_control_restored(0U);

    reset_fake();
    make_packet(packet);
    memcpy(bios, disk[0], 512U);
    CHECK(cvata_prepare(packet) == CVATA_OK);
    cvata_observe_control(0U, 1U);
    identify_data[106U * 2U] = 0x00U;
    identify_data[106U * 2U + 1U] = 0x50U; /* valid word 106, logical > 256 words */
    identify_data[117U * 2U] = 0x00U;
    identify_data[117U * 2U + 1U] = 0x02U; /* 512 words = 1024-byte sectors */
    CHECK(cvata_bind(bios) == CVATA_E_UNSUPPORTED);
    check_control_restored(0U);

    reset_fake();
    make_packet(packet);
    memcpy(bios, disk[0], 512U);
    CHECK(cvata_prepare(packet) == CVATA_OK);
    cvata_observe_control(0U, 1U);
    identify_data[0] |= 0x80U; /* removable media device */
    CHECK(cvata_bind(bios) == CVATA_E_UNSUPPORTED);
    check_control_restored(0U);
}

static void test_error_and_quarantine(void)
{
    uint8_t bios[512], buffer[512];
    uint32_t before, control_before;
    reset_fake();
    prepare_bound(bios);
    fail_command = ATA_READ;
    before = command_count;
    CHECK(cvata_transfer(0U, 2U, buffer) == CVATA_E_ATA);
    CHECK(command_count == before + 1U);
    CHECK(cvata_get_status()->state == CVATA_STATE_BOUND);
    check_control_restored(0U);

    reset_fake();
    prepare_bound(bios);
    command_stuck_mode = MODE_STUCK_BUSY;
    before = command_count;
    control_before = device_control_writes;
    CHECK(cvata_transfer(0U, 2U, buffer) == CVATA_E_QUARANTINED);
    CHECK(command_count == before + 1U);
    CHECK(cvata_get_status()->state == CVATA_STATE_QUARANTINED);
    CHECK((device_control_value & ATA_CTL_NIEN) != 0U);
    CHECK(device_control_writes == control_before + 1U); /* nIEN retained, no restore */
    CHECK(bad_control_restore_count == 0U);
    before = command_count;
    CHECK(cvata_transfer(0U, 2U, buffer) == CVATA_E_QUARANTINED);
    CHECK(command_count == before);

    {
        uint8_t packet[64];
        uint32_t result;
        make_packet(packet);
        result = cvata_prepare(packet);
        CHECK(result == CVATA_E_QUARANTINED);
        CHECK(cvata_get_status()->state == CVATA_STATE_QUARANTINED);
        CHECK(cvata_transfer(0U, 2U, buffer) == CVATA_E_QUARANTINED);
        CHECK(command_count == before);
        CHECK(device_control_writes == control_before + 1U);
        CHECK((device_control_value & ATA_CTL_NIEN) != 0U);
    }
}

static void test_time_bounded_polling(void)
{
    uint8_t bios[512], buffer[512], packet[64];
    uint32_t before_alt, before_clock, before_commands, before_control;
    uint64_t before_ticks;

    reset_fake();
    make_packet(packet);
    fake_clock_frequency = 0U;
    CHECK(cvata_prepare(packet) == CVATA_E_UNSUPPORTED);
    CHECK(cvata_get_status()->state == CVATA_STATE_UNBOUND);
    CHECK(command_count == 0U && ata_io_count == 0U);
    CHECK(fake_clock_samples == 0U);

    reset_fake();
    prepare_bound(bios);
    command_status_delay_config = 70005U;
    fake_clock_step = 1U; /* one cycle/sample keeps this >65k polls under 30 s */
    before_alt = alternate_status_reads;
    before_clock = fake_clock_samples;
    before_ticks = fake_clock_ticks;
    CHECK(cvata_transfer(0U, 3U, buffer) == CVATA_OK);
    long_poll_status_samples = alternate_status_reads - before_alt;
    long_poll_clock_samples = fake_clock_samples - before_clock;
    CHECK(long_poll_status_samples > 65536U);
    CHECK(long_poll_clock_samples > 65536U);
    CHECK(fake_clock_ticks - before_ticks <
          (uint64_t)g_clock_khz * ATA_TIMEOUT_MS);
    CHECK(cvata_get_status()->state == CVATA_STATE_BOUND);
    check_control_restored(0U);

    reset_fake();
    prepare_bound(bios);
    fake_clock_ticks = 0xffffff00ULL;
    fake_clock_last_high = 0U;
    fake_clock_step = 0x40U;
    command_status_delay_config = 8U; /* cross low-word wrap while waiting */
    CHECK(cvata_transfer(0U, 3U, buffer) == CVATA_OK);
    rollover_observed = fake_clock_high_transitions;
    CHECK(rollover_observed > 0U);
    check_control_restored(0U);

    reset_fake();
    prepare_bound(bios);
    fake_clock_ticks = 0xfffffffffffff000ULL;
    fake_clock_last_high = 0xffffffffU;
    fake_clock_step = 0x100U;
    command_status_delay_config = 32U;
    CHECK(cvata_transfer(0U, 3U, buffer) == CVATA_OK);
    full_wrap_observed = fake_clock_full_wraps;
    CHECK(full_wrap_observed > 0U);
    check_control_restored(0U);

    reset_fake();
    prepare_bound(bios);
    command_stuck_mode = MODE_STUCK_BUSY;
    fake_clock_step = (uint64_t)g_clock_khz * ATA_TIMEOUT_MS;
    before_clock = fake_clock_samples;
    before_commands = command_count;
    before_control = device_control_writes;
    CHECK(cvata_transfer(0U, 3U, buffer) == CVATA_E_QUARANTINED);
    timeout_clock_samples = fake_clock_samples - before_clock;
    CHECK(command_count == before_commands + 1U);
    CHECK(timeout_clock_samples < 20U);
    CHECK(cvata_get_status()->state == CVATA_STATE_QUARANTINED);
    CHECK(device_control_writes == before_control + 1U);
    CHECK((device_control_value & ATA_CTL_NIEN) != 0U);

    reset_fake();
    prepare_bound(bios);
    command_stuck_mode = MODE_STUCK_BUSY;
    fake_clock_step = 0U; /* frozen clock: bounded stagnation escape */
    before_clock = fake_clock_samples;
    before_commands = command_count;
    CHECK(cvata_transfer(0U, 3U, buffer) == CVATA_E_QUARANTINED);
    frozen_clock_samples = fake_clock_samples - before_clock;
    CHECK(command_count == before_commands + 1U);
    CHECK(frozen_clock_samples >= ATA_STALLED_CLOCK_LIMIT + 2U);
    CHECK(frozen_clock_samples <= ATA_STALLED_CLOCK_LIMIT + 20U);
    CHECK(cvata_get_status()->state == CVATA_STATE_QUARANTINED);
    CHECK((device_control_value & ATA_CTL_NIEN) != 0U);
}

static void test_deadline_product_matches_64bit_reference(void)
{
    static const uint32_t frequencies[] = {
        1000U, 1000000U, 20000000U, 0xffffffffU
    };
    uint8_t packet[64];
    uint32_t i;
    for (i = 0U; i < sizeof frequencies / sizeof frequencies[0]; ++i) {
        uint64_t expected;
        reset_fake();
        make_packet(packet);
        fake_clock_frequency = frequencies[i];
        CHECK(cvata_prepare(packet) == CVATA_OK);
        expected = (uint64_t)frequencies[i] * (uint64_t)ATA_TIMEOUT_MS;
        CHECK(g_timeout_low == (uint32_t)expected);
        CHECK(g_timeout_high == (uint32_t)(expected >> 32));
        CHECK(command_count == 0U && ata_io_count == 0U);
        ++deadline_product_cases;
    }
    CHECK(g_timeout_high != 0U); /* max-frequency product carries above 32 bits */
}

static void discovery_packet(uint8_t packet[64])
{
    make_packet(packet);
    packet[8] = 26U;
    memset(packet + 8U + 26U, 0xff, 4U); /* unavailable BIOS DPTE pointer */
    memset(packet + 40U, 0, 16U);
}

static void discovery_tuple(uint32_t base, uint8_t head, uint8_t count,
                            uint8_t low, uint8_t mid, uint8_t high,
                            uint8_t command)
{
    cvata_discovery_io(base + ATA_DEVICE, head, 1U);
    cvata_discovery_io(base + ATA_COUNT, count, 1U);
    cvata_discovery_io(base + ATA_LBA_LOW, low, 1U);
    cvata_discovery_io(base + ATA_LBA_MID, mid, 1U);
    cvata_discovery_io(base + ATA_LBA_HIGH, high, 1U);
    cvata_discovery_io(base + ATA_COMMAND, command, 1U);
}

static void check_observation_passive(void)
{
    CHECK(command_count == 0U && ata_io_count == 0U);
    CHECK(device_control_writes == 0U && fake_clock_samples == 0U);
}

static void test_firmware_discovery_supported_reads(void)
{
    static const uint8_t commands[] = {0x20U, 0xc4U, 0xc8U};
    uint8_t packet[64], original[64], bios[512];
    uint32_t i, j, sum;
    for (i = 0U; i < sizeof commands; ++i) {
        reset_fake();
        discovery_packet(packet);
        memcpy(original, packet, 64U);
        CHECK(cvata_discovery_begin(packet) == CVATA_OK);
        cvata_discovery_io(PRIMARY_CTL, 0x0cU, 1U); /* actual BIOS SRST */
        cvata_discovery_io(PRIMARY_CTL, 0x08U, 1U); /* actual deassert */
        discovery_tuple(PRIMARY_CMD, 0xe0U, 1U, 0U, 0U, 0U, commands[i]);
        check_observation_passive();
        CHECK(cvata_discovery_end(packet) == CVATA_OK);
        CHECK(get16(packet + 40U) == PRIMARY_CMD);
        CHECK(get16(packet + 42U) == PRIMARY_CTL);
        CHECK(packet[44] == 0xe0U && packet[46] == 14U);
        CHECK(get16(packet + 8U) == 26U); /* raw firmware length retained */
        CHECK(get32(packet + 8U + 26U) == 0xffffffffU);
        CHECK(memcmp(packet, original, 40U) == 0);
        CHECK(memcmp(packet + 56U, original + 56U, 8U) == 0);
        sum = 0U;
        for (j = 40U; j < 56U; ++j) sum += packet[j];
        CHECK((sum & 0xffU) == 0U);
        CHECK(g_status.state == CVATA_STATE_PREPARED);
        CHECK(g_status.control_shadow_known == 1U && g_status.control_shadow == 8U);
        check_observation_passive();
        device_control_value = 8U;
        memcpy(bios, disk[0], 512U);
        CHECK(cvata_bind(bios) == CVATA_OK); /* full native identity/sector proof */
        CHECK(command_count == 2U && g_status.state == CVATA_STATE_BOUND);
        check_control_restored(8U);
    }

    reset_fake();
    discovery_packet(packet);
    CHECK(cvata_discovery_begin(packet) == CVATA_OK);
    /* BIOS reset setup of either channel does not establish a disk route. */
    cvata_discovery_io(0x376U, 0x04U, 1U);
    cvata_discovery_io(0x376U, 0U, 1U);
    discovery_tuple(0x170U, 0xa0U, 4U, 0U, 0U, 0U, 0xc6U);
    cvata_discovery_io(PRIMARY_CTL, 2U, 1U);
    discovery_tuple(PRIMARY_CMD, 0xf0U, 1U, 0U, 0U, 0U, 0x20U);
    CHECK(cvata_discovery_end(packet) == CVATA_OK);
    CHECK(g_status.command_base == PRIMARY_CMD && g_status.device_head == 0xf0U);
    CHECK(g_status.control_shadow == 2U);
    check_observation_passive();

    reset_fake();
    discovery_packet(packet);
    CHECK(cvata_discovery_begin(packet) == CVATA_OK);
    cvata_discovery_io(0x376U, 8U, 1U);
    discovery_tuple(0x170U, 0xf0U, 1U, 0U, 0U, 0U, 0xc8U);
    CHECK(cvata_discovery_end(packet) == CVATA_OK);
    CHECK(g_status.command_base == 0x170U && g_status.control_base == 0x376U);
    CHECK(g_status.device_head == 0xf0U && g_status.irq == 15U);
    check_observation_passive();
}

static void test_firmware_discovery_rejects_ambiguity(void)
{
    static const uint8_t bad_commands[] = {0x30U, 0xcaU, 0x24U, 0x25U, 0xc7U, 0x40U};
    uint8_t packet[64], original[64];
    uint32_t i;
    for (i = 0U; i < sizeof bad_commands; ++i) {
        reset_fake(); discovery_packet(packet); memcpy(original, packet, 64U);
        CHECK(cvata_discovery_begin(packet) == CVATA_OK);
        cvata_discovery_io(PRIMARY_CTL, 0U, 1U);
        discovery_tuple(PRIMARY_CMD, 0xe0U, 1U, 0U, 0U, 0U, bad_commands[i]);
        CHECK(cvata_discovery_end(packet) == CVATA_E_UNSUPPORTED);
        CHECK(memcmp(packet, original, 64U) == 0); /* no invented mapping */
        CHECK(g_status.state == CVATA_STATE_UNBOUND);
        check_observation_passive();
    }
    /* Each count/address/head field has to be written and valid. */
    for (i = 0U; i < 7U; ++i) {
        reset_fake(); discovery_packet(packet);
        CHECK(cvata_discovery_begin(packet) == CVATA_OK);
        cvata_discovery_io(PRIMARY_CTL, 0U, 1U);
        discovery_tuple(PRIMARY_CMD, i == 5U ? 0xa0U : i == 6U ? 0xe1U : 0xe0U,
                        i == 0U ? 0U : i == 1U ? 2U : 1U,
                        i == 2U ? 1U : 0U, i == 3U ? 1U : 0U,
                        i == 4U ? 1U : 0U, 0x20U);
        CHECK(cvata_discovery_end(packet) == CVATA_E_UNSUPPORTED);
        check_observation_passive();
    }
    for (i = 0U; i < 5U; ++i) {
        uint32_t j;
        reset_fake(); discovery_packet(packet);
        CHECK(cvata_discovery_begin(packet) == CVATA_OK);
        cvata_discovery_io(PRIMARY_CTL, 0U, 1U);
        for (j = 0U; j < 5U; ++j)
            if (j != i) cvata_discovery_io(PRIMARY_CMD + 2U + j,
                            j == 0U ? 1U : j == 4U ? 0xe0U : 0U, 1U);
        cvata_discovery_io(PRIMARY_CMD + ATA_COMMAND, 0x20U, 1U);
        CHECK(cvata_discovery_end(packet) == CVATA_E_UNSUPPORTED);
        check_observation_passive();
    }
    reset_fake(); discovery_packet(packet);
    CHECK(cvata_discovery_begin(packet) == CVATA_OK);
    cvata_discovery_io(PRIMARY_CTL, 0U, 1U);
    for (i = 0U; i < 5U; ++i)
        cvata_discovery_io(PRIMARY_CMD + 2U + i,
                          i == 0U ? 1U : i == 4U ? 0xe0U : 0U, 1U);
    cvata_discovery_io(0x177U, 0xc6U, 1U); /* any setup invalidates stale tuple */
    cvata_discovery_io(PRIMARY_CMD + ATA_COMMAND, 0x20U, 1U);
    CHECK(cvata_discovery_end(packet) == CVATA_E_UNSUPPORTED);
    check_observation_passive();
    for (i = 0U; i < 10U; ++i) {
        reset_fake(); discovery_packet(packet);
        CHECK(cvata_discovery_begin(packet) == CVATA_OK);
        if (i != 0U) cvata_discovery_io(PRIMARY_CTL, 0U, 1U);
        discovery_tuple(PRIMARY_CMD, 0xe0U, 1U, 0U, 0U, 0U, 0x20U);
        if (i == 1U) {
            cvata_discovery_io(0x376U, 0U, 1U);
            discovery_tuple(0x170U, 0xe0U, 1U, 0U, 0U, 0U, 0x20U);
        }
        if (i == 2U) discovery_tuple(PRIMARY_CMD, 0xf0U, 1U, 0U, 0U, 0U, 0x20U);
        if (i == 3U) cvata_discovery_io(PRIMARY_CTL, ATA_CTL_SRST, 1U);
        if (i == 4U) cvata_discovery_io(PRIMARY_CTL, ATA_CTL_HOB, 1U);
        if (i == 5U) cvata_discovery_io(PRIMARY_CMD + ATA_COUNT, 1U, 2U);
        if (i == 6U) cvata_discovery_io(PRIMARY_CTL, 0U, 2U);
        if (i == 7U) cvata_discovery_io(PRIMARY_CMD + ATA_COMMAND, 0xc6U, 1U);
        if (i == 8U) cvata_discovery_io(PRIMARY_CMD + ATA_COMMAND, 0x20U, 1U);
        if (i == 9U) cvata_discovery_io(0x172U, 1U, 1U);
        CHECK(cvata_discovery_end(packet) == CVATA_E_UNSUPPORTED);
        check_observation_passive();
    }
}

static void test_firmware_discovery_lifecycle(void)
{
    uint8_t packet[64], bios[512];
    uint32_t i;
    reset_fake(); discovery_packet(packet);
    CHECK(cvata_discovery_end(packet) == CVATA_UNBOUND);
    CHECK(cvata_discovery_begin(0) == CVATA_E_ARGUMENT);
    CHECK(cvata_discovery_begin(packet) == CVATA_OK);
    cvata_discovery_cancel(); cvata_discovery_cancel();
    cvata_discovery_io(PRIMARY_CTL, 0U, 1U);
    discovery_tuple(PRIMARY_CMD, 0xe0U, 1U, 0U, 0U, 0U, 0x20U);
    CHECK(cvata_discovery_end(packet) == CVATA_UNBOUND);
    CHECK(g_status.command_base == 0U && g_status.capacity_sectors == 0U);
    check_observation_passive();
    for (i = 0U; i < 6U; ++i) {
        reset_fake(); discovery_packet(packet);
        if (i == 0U) packet[8] = 25U;
        if (i == 1U) packet[8 + 25U] = 4U;
        if (i == 2U) packet[8 + 16U] = 0U;
        if (i == 3U) packet[56U] = 0U;
        if (i == 4U) fake_clock_frequency = 0U;
        if (i == 5U) packet[8 + 2U] = 4U; /* removable BIOS disk */
        CHECK(cvata_discovery_begin(packet) == CVATA_E_UNSUPPORTED);
        CHECK(cvata_discovery_end(packet) == CVATA_UNBOUND);
        check_observation_passive();
    }
    reset_fake(); discovery_packet(packet);
    CHECK(cvata_discovery_begin(packet) == CVATA_OK);
    cvata_discovery_io(PRIMARY_CTL, 0U, 1U);
    discovery_tuple(PRIMARY_CMD, 0xe0U, 1U, 0U, 0U, 0U, 0x20U);
    ++packet[6]; /* cannot change the BIOS drive after observing its read */
    CHECK(cvata_discovery_end(packet) == CVATA_E_PACKET);
    check_observation_passive();
    reset_fake(); prepare_bound(bios); discovery_packet(packet);
    CHECK(cvata_discovery_begin(packet) == CVATA_E_BUSY);
    cvata_discovery_cancel();
    CHECK(g_status.state == CVATA_STATE_BOUND && g_status.ready == 1U);
    g_control_owned = 1U; g_status.state = CVATA_STATE_QUARANTINED;
    CHECK(cvata_discovery_begin(packet) == CVATA_E_QUARANTINED);
    cvata_discovery_cancel();
    CHECK(g_control_owned == 1U && g_status.state == CVATA_STATE_QUARANTINED);
}

static void test_firmware_discovery_reset_setup_and_bind_proof(void)
{
    static const uint8_t setup[] = {0xc6U, 0x91U, 0xefU, 0xecU, 0x08U};
    uint8_t packet[64], bios[512];
    uint32_t i;
    for (i = 0U; i < sizeof setup; ++i) {
        reset_fake(); discovery_packet(packet);
        CHECK(cvata_discovery_begin(packet) == CVATA_OK);
        cvata_discovery_io(PRIMARY_CTL, 0U, 1U);
        discovery_tuple(PRIMARY_CMD, 0xa0U, 4U, 0U, 0U, 0U, setup[i]);
        CHECK(cvata_discovery_end(packet) == CVATA_E_UNSUPPORTED);
        check_observation_passive(); /* setup alone never identifies boot drive */

        reset_fake(); discovery_packet(packet);
        CHECK(cvata_discovery_begin(packet) == CVATA_OK);
        cvata_discovery_io(PRIMARY_CTL, 0U, 1U);
        discovery_tuple(PRIMARY_CMD, 0xa0U, 4U, 0U, 0U, 0U, setup[i]);
        discovery_tuple(PRIMARY_CMD, 0xe0U, 1U, 0U, 0U, 0U, 0x20U);
        CHECK(cvata_discovery_end(packet) == CVATA_OK);
        check_observation_passive();

        reset_fake(); discovery_packet(packet);
        CHECK(cvata_discovery_begin(packet) == CVATA_OK);
        cvata_discovery_io(PRIMARY_CTL, 0U, 1U);
        discovery_tuple(PRIMARY_CMD, 0xe0U, 1U, 0U, 0U, 0U, 0x20U);
        cvata_discovery_io(PRIMARY_CMD + ATA_COMMAND, setup[i], 1U);
        CHECK(cvata_discovery_end(packet) == CVATA_E_UNSUPPORTED);
        check_observation_passive();
    }
    reset_fake(); discovery_packet(packet);
    CHECK(cvata_discovery_begin(packet) == CVATA_OK);
    cvata_discovery_io(PRIMARY_CTL, 0U, 1U);
    discovery_tuple(PRIMARY_CMD, 0xe0U, 1U, 0U, 0U, 0U, 0xc8U);
    discovery_tuple(PRIMARY_CMD, 0xe0U, 1U, 0U, 0U, 0U, 0xc8U);
    CHECK(cvata_discovery_end(packet) == CVATA_OK); /* identical fresh BIOS retry */
    memcpy(bios, disk[0], 512U); bios[511] ^= 1U;
    CHECK(cvata_bind(bios) == CVATA_E_UNSUPPORTED);
    CHECK(g_status.state == CVATA_STATE_PREPARED && g_status.ready == 0U);
    CHECK(command_count == 2U);
    check_control_restored(0U);
}

static void test_legacy_dpte_revision_preserves_validation(void)
{
    /* Official Lenovo 1auj20us.exe BIOS code module 0, offsets
     * 602d/603d/604d/605d. These ROM templates prove the legacy layout, not
     * the live boot disk's mapping or its enabled LBA translation mode. */
    static const uint8_t templates[4][16] = {
        {0xf0,0x01,0xf6,0x03,0xa0,0x00,0x0e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x58},
        {0xf0,0x01,0xf6,0x03,0xb0,0x00,0x0e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x48},
        {0x70,0x01,0x76,0x03,0xa0,0x00,0x0f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x57},
        {0x70,0x01,0x76,0x03,0xb0,0x00,0x0f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x47}
    };
    uint8_t packet[64], bios[512], buffer[512], written[512];
    uint32_t i, j, sum;
    for (i = 0U; i < 4U; ++i) {
        reset_fake(); make_packet(packet);
        memcpy(packet + ADP_DPT, templates[i], 16U);
        sum = 0U;
        for (j = 0U; j < 16U; ++j) sum += templates[i][j];
        CHECK((sum & 0xffU) == 0U && packet[54] == 0x10U);
        CHECK(cvata_prepare(packet) == CVATA_E_UNSUPPORTED); /* no live LBA flag */
        CHECK(g_status.state == CVATA_STATE_UNBOUND && g_status.ready == 0U);
        check_observation_passive();
        /* A hypothetical *live BIOS* table with LBA explicitly enabled.
         * The driver must never make this transformation to a ROM table. */
        packet[44] |= 0x40U;
        packet[50] |= 0x10U;
        refresh_checksum(packet);
        CHECK(cvata_prepare(packet) == CVATA_OK);
        CHECK(g_status.command_base == (i < 2U ? 0x1f0U : 0x170U));
        CHECK(g_status.control_base == (i < 2U ? 0x3f6U : 0x376U));
        CHECK(g_status.device_head == ((i & 1U) ? 0xf0U : 0xe0U));
        CHECK(g_status.irq == (i < 2U ? 14U : 15U));
        check_observation_passive();
    }
    for (i = 0U; i < 2U; ++i) {
        reset_fake(); make_packet(packet);
        packet[54] = 0x10U;
        packet[44] = i == 0U ? 0xe0U : 0xf0U;
        refresh_checksum(packet);
        memcpy(bios, disk[0], 512U);
        CHECK(cvata_prepare(packet) == CVATA_OK);
        CHECK(cvata_bind(bios) == CVATA_E_UNSUPPORTED); /* control still unknown */
        check_observation_passive();
        selected_device_head = target_device_head = packet[44];
        device_control_value = 8U;
        cvata_observe_control(8U, 1U);
        CHECK(cvata_bind(bios) == CVATA_OK);
        CHECK(command_count == 2U); /* native IDENTIFY + complete LBA0 */
        CHECK(cvata_transfer(0U, 3U, buffer) == CVATA_OK);
        CHECK(memcmp(buffer, disk[3], 512U) == 0);
        memset(written, 0x5a, 512U);
        CHECK(cvata_transfer(1U, 5U, written) == CVATA_OK);
        CHECK(cvata_transfer(0U, 5U, buffer) == CVATA_OK);
        CHECK(memcmp(written, buffer, 512U) == 0);
        CHECK(g_status.state == CVATA_STATE_BOUND && g_status.ready == 1U);
        check_control_restored(8U);
    }
    for (i = 0U; i < 11U; ++i) {
        reset_fake(); make_packet(packet); packet[54] = 0x10U;
        if (i == 0U) packet[50] = 0U;
        if (i == 1U) packet[50] |= 0x20U;
        if (i == 2U) packet[50] |= 0x40U;
        if (i == 3U) packet[40] = 0x68U;
        if (i == 4U) packet[42] = 0x76U;
        if (i == 5U) packet[46] = 15U;
        if (i == 6U) packet[44] = 0x80U;
        if (i == 7U) packet[24] = 0U;
        if (i == 8U) packet[33] = 4U;
        if (i == 9U) packet[56] = 0U;
        if (i == 10U) fake_clock_frequency = 0U;
        refresh_checksum(packet);
        CHECK(cvata_prepare(packet) == CVATA_E_UNSUPPORTED);
        check_observation_passive();
    }
    reset_fake(); make_packet(packet); packet[54] = 0x10U;
    refresh_checksum(packet); packet[55] ^= 1U;
    CHECK(cvata_prepare(packet) == CVATA_E_PACKET);
    check_observation_passive();
    reset_fake(); make_packet(packet); packet[54] = 0x10U;
    refresh_checksum(packet); memcpy(bios, disk[0], 512U); bios[511] ^= 1U;
    CHECK(cvata_prepare(packet) == CVATA_OK);
    cvata_observe_control(0U, 1U);
    CHECK(cvata_bind(bios) == CVATA_E_UNSUPPORTED);
    CHECK(g_status.state == CVATA_STATE_PREPARED && g_status.ready == 0U);
    CHECK(command_count == 2U);
    check_control_restored(0U);
}

int main(void)
{
    test_validation_has_no_ata_commands();
    test_busy_pic_has_no_command();
    test_firmware_control_preserved_and_error_cleanup();
    test_stale_error_and_preexisting_busy();
    test_empty_previous_selection_before_target_select();
    test_identify_capacity_and_sector_size();
    test_error_and_quarantine();
    test_time_bounded_polling();
    test_deadline_product_matches_64bit_reference();
    test_firmware_discovery_supported_reads();
    test_firmware_discovery_rejects_ambiguity();
    test_firmware_discovery_lifecycle();
    test_firmware_discovery_reset_setup_and_bind_proof();
    test_legacy_dpte_revision_preserves_validation();
    test_bind_and_sector_io();
    printf("{\"test\":\"session_disk_ata\",\"checks\":%u,"
           "\"regular_status_reads\":%u,\"alternate_status_reads\":%u,"
           "\"status_trace_stored\":%u,\"device_control_writes\":%u,"
           "\"bad_control_restores\":%u,\"bulk_read_calls\":%u,"
           "\"bulk_read_words\":%u,\"bulk_write_calls\":%u,"
           "\"bulk_write_words\":%u,\"direct_data_words\":%u,"
           "\"long_poll_status_samples\":%u,\"long_poll_clock_samples\":%u,"
           "\"timeout_clock_samples\":%u,\"frozen_clock_samples\":%u,"
           "\"rollover_observed\":%u,\"full_wrap_observed\":%u,"
           "\"deadline_product_cases\":%u}\n",
           checks, regular_status_reads, alternate_status_reads,
           status_trace_count, device_control_writes, bad_control_restore_count,
           bulk_read_calls, bulk_read_words, bulk_write_calls, bulk_write_words,
           direct_data_read_words + direct_data_write_words,
           long_poll_status_samples, long_poll_clock_samples,
           timeout_clock_samples, frozen_clock_samples, rollover_observed,
           full_wrap_observed, deadline_product_cases);
    return 0;
}
