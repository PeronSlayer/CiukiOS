#!/usr/bin/env python3
"""Decode the bounded CVFL launch checkpoint written by a child VMFORK."""
import argparse
import json
from pathlib import Path
import struct

STAGES = {1: 'before_preparation', 2: 'preparation_complete', 3: 'compaction_entered',
          4: 'ancestor_cleanup_entered', 5: 'ancestor_cleanup_complete',
          6: 'target_exec_entered', 7: 'target_exec_error', 8: 'target_returned',
          9: 'optional_ancestor_free_refused', 10: 'preparation_failed',
          11: 'compaction_failed', 12: 'mandatory_cleanup_failed',
          13: 'inherited_vectors_restored', 14: 's3_guest_detached',
          15: 'child_entered_before_memory_probe',
          64: 'dpmirun_api_found', 65: 'firmware_fonts_entered',
          66: 'video_configuration_entered', 67: 'video_session_begin_entered',
          68: 'video_session_begun', 69: 'device_session_begin_entered',
          70: 'device_session_begin_returned', 71: 'virtual_mode_set_entered',
          72: 'virtual_mode_set_returned', 73: 'dpmi_host_exec_entered',
          74: 'dpmi_host_exec_returned', 75: 'command_exec_entered',
          76: 'command_exec_returned', 77: 'command_exit_code_observed',
          78: 'dpmi_host_exit_code_observed'}
S3_REASONS = {0x80: 's3_handler_or_active_call', 0x81: 's3_mcb_or_extent',
              0x82: 's3_auxstack_predecessor', 0x83: 's3_arena_membership',
              0x84: 's3_other_vector_reference', 0x85: 's3_private_free_failed'}
REASONS = dict(S3_REASONS)
REASONS.update({0x3f: 'ancestor_vector_reference', 0x45: 'lfn_signature_word',
                0x46: 'lfn_immediate_byte', 0x47: 'resident_fallback_alloc_failed',
                0x48: 'resident_fallback_not_below_source',
                0x57: 'rebased_psp_resize_failed'})


def parse_launch(data):
    if len(data) != 256 or data[:4] != b'CVFL':
        raise ValueError('CVFL needs its magic and exactly 256 bytes')
    word = lambda offset: struct.unpack_from('<H', data, offset)[0]
    if word(4) != 1 or word(6) != 256:
        raise ValueError('unsupported CVFL version or declared size')
    if not 1 <= word(8) <= 3 or not word(10):
        raise ValueError('invalid CVFL VM id or generation')
    stage, reason, progress = word(12), word(14), word(16)
    target = data[64:192]
    if b'\0' not in target:
        raise ValueError('unterminated CVFL target pathname')
    return dict(format='CVFL', version=1, size_bytes=256, vm=word(8),
                generation=word(10), stage=stage, stage_name=STAGES.get(stage, 'unknown'),
                reason=reason, reason_name=REASONS.get(reason), progress=progress,
                target=target.split(b'\0', 1)[0].decode('cp437'),
                target_exec_attempted=bool(progress & 0x40),
                target_exec_returned=bool(progress & 0x100),
                last_dos_ax=word(18), last_dos_flags=word(20),
                last_dos_carry=bool(word(20) & 1), cs=word(22), psp=word(24),
                root_psp=word(26), ancestor_count=word(28),
                conventional_memory_kib=word(30),
                largest_free_before_mutation_paragraphs=word(32),
                memory_walk_complete=word(34) == 1, vector=word(36), exit_code=word(38),
                checkpoint_sequence=struct.unpack_from('<I', data, 40)[0],
                previous_diagnostic_io_error=word(44), cleanup_reason=word(46),
                dpmi_host=(dict(exec_attempted=bool(data[254] & 1),
                    exec_ax=word(248), exec_flags=word(250),
                    exec_carry=bool(word(250) & 1),
                    exit_observed=bool(data[254] & 2), exit_ax=word(252),
                    exit_code=data[252], termination_type=data[253],
                    direct_command=bool(data[255]),
                    real_mode_declared=bool(data[254] & 4))
                    if data[246:248] == b'DP' else None),
                ivt_baseline=(dict(registered=word(248) == 1,
                    free_target_count=word(250), filtered_target_count=word(252),
                    first_filtered_vector_byte_offset=word(254))
                    if data[246:248] == b'IV' else None),
                offending_vector_byte_offset=word(48),
                s3_private_copy_detached=bool(progress & 0x400), s3_psp=word(50),
                s3_previous_int10=dict(offset=word(52), segment=word(54)),
                s3_resident_paragraphs=word(56),
                inspected_vector_target=dict(offset=word(58), segment=word(60)),
                observed_lfn_immediate=word(62),
                parent_persisted_transport=data[214:218] == b'PLOG',
                root_allocation_retained=bool(progress & 0x800),
                ancestor_allocation_retained=bool(progress & 0x1000),
                pin_observations=word(192), first_pinned_mcb=word(194),
                first_pinned_owner=word(196), first_pinned_paragraphs=word(198),
                first_pinned_vector_byte_offset=word(200),
                first_pinned_target=dict(offset=word(202), segment=word(204)),
                initial_free_paragraphs=word(206), remaining_free_paragraphs=word(208),
                remaining_largest_free_paragraphs=word(210),
                remaining_memory_walk_complete=word(212) == 1,
                native_disk=(dict(state=word(218), error=word(220),
                    command_port=word(222), control_port=word(224),
                    read_count=struct.unpack_from('<I', data, 226)[0],
                    write_count=struct.unpack_from('<I', data, 230)[0],
                    last_lba=struct.unpack_from('<I', data, 234)[0],
                    control_shadow=data[238], control_known=bool(data[239]),
                    binding_phase=data[241], binding_error=data[240])
                    if data[242:246] == b'ATA1' else None))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('snapshot', type=Path)
    args = parser.parse_args()
    print(json.dumps(parse_launch(args.snapshot.read_bytes()), indent=2))


if __name__ == '__main__':
    main()
