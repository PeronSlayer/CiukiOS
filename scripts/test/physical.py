"""Bounded import of operator-confirmed F0 serial/screen transcriptions.

This imports evidence only. It does not mount or write a disk, normalize records,
claim UART qualification, or substitute historical firmware replay for T4.
"""
import hashlib
from pathlib import Path
from evidence import Parser, EvidenceError
from loader_model import selector

LIMIT=128*1024
FIELDS=('model','unit_identity','bios_version','cpuid','installed_ram','pci_ids',
        'target_disk_identity','write_sha256','readback_sha256','capture_settings')


def import_evidence(capture,canonical_hash,expected):
    import json
    capture=Path(capture)
    meta=capture/'acquisition.json';log=capture/'f0.log'
    if meta.stat().st_size>LIMIT or log.stat().st_size>LIMIT:
        raise EvidenceError('physical acquisition exceeds 128 KiB allowlist limit')
    metadata=json.loads(meta.read_text())
    if metadata.get('operator_confirmed') is not True:
        raise EvidenceError('physical model/unit identity needs operator confirmation')
    selection=selector(metadata['selector'],metadata.get('selector_source','menu'))
    if selection['probe']=='all':raise EvidenceError('import one probe per capture')
    missing=[key for key in FIELDS if key not in metadata]
    if missing:raise EvidenceError('missing physical identity fields: '+','.join(missing))
    if metadata['write_sha256']!=canonical_hash or metadata['readback_sha256']!=canonical_hash:
        raise EvidenceError('physical write/readback does not match canonical image')
    if metadata.get('build_id','unknown')=='unknown':raise EvidenceError('embedded build identity missing')
    if metadata.get('disk_log','unavailable')!='unavailable' and not metadata.get('storage_qualified',False):
        raise EvidenceError('unqualified storage log')
    parser=Parser(selection['run'],selection['probe'])
    raw=log.read_bytes()
    for line in raw.splitlines(keepends=True):parser.feed(line)
    parser.check(expected)
    build_records=[r for r in parser.records if r.get('build_id')==metadata['build_id']]
    if not build_records:raise EvidenceError('physical records do not identify the verified build')
    if expected['terminal']=='PANIC' and (metadata.get('external_halt_seconds',0)<5 or metadata.get('resumed',True)):
        raise EvidenceError('physical panic lacks five-second halt observation')
    return {'schema_version':1,'run_id':selection['run'],'probe':selection['probe'],
            'suite':metadata.get('suite','unknown'),'utc_start':metadata.get('utc_start','unknown'),
            'utc_end':metadata.get('utc_end','unknown'),'image_size':metadata.get('image_size','unknown'),
            'build_manifest_hash':metadata.get('build_manifest_hash','unknown'),
            'build_git_revision':metadata.get('build_git_revision','unknown'),'build_dirty':metadata.get('build_dirty','unknown'),
            'runner_revision':metadata.get('runner_revision','unknown'),
            'timeout':{'occurred':False,'external_seconds':metadata.get('external_seconds','unknown')},
            'cleanup':{'kind':'read-only evidence import','children':0},
            'outcome':'pass','reason':'operator-confirmed capture satisfies declared predicates',
            'selector':metadata['selector'],'image_sha256':canonical_hash,'physical':metadata,
            'expected':expected,'observed':parser.records,'disk_log':metadata.get('disk_log','unavailable'),
            'artifact_hashes':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (meta,log)}}
