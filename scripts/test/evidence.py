"""Strict controller evidence and separately framed untrusted application bytes."""
import hashlib
import json
import re
from loader_model import PROBES


class EvidenceError(ValueError):
    pass


class ApplicationCapture:
    """Scan/hash the complete stream, retaining a bounded head and tail only.

    Incremental hashing follows https://docs.python.org/3/library/hashlib.html.
    No application byte is fed back into Parser, including CIUKI_TEST text.
    """
    def __init__(self, retention=65536, log_limit=4*1024**2):
        self.retention = retention
        self.log_limit = log_limit
        self.total_bytes = 0
        self.digest = hashlib.sha256()
        self.head = bytearray()
        self.tail = bytearray()
        self.identities = set()
        self.scan_tail = b''
        self.final_success_indication = False
        self.assertion_indications = 0

    def feed(self, data, tid=None, stream=None, pid=None):
        if tid is not None:
            self.identities.add((pid, tid, stream))
            if len(self.identities) > 64:
                raise EvidenceError('application stream identity limit exceeded')
        self.digest.update(data)
        self.total_bytes += len(data)
        if self.total_bytes <= self.log_limit:
            self.head.extend(data)
        else:
            del self.head[self.retention:]
            self.head.extend(data[:max(0, self.retention-len(self.head))])
        if len(data) >= self.retention:
            self.tail[:] = data[-self.retention:]
        else:
            self.tail.extend(data)
            if len(self.tail) > self.retention:del self.tail[:-self.retention]
        scanned = self.scan_tail + data
        self.final_success_indication |= b'final OK' in scanned
        token = b'assertion failed!'
        self.assertion_indications += scanned.count(token)-self.scan_tail.count(token)
        self.scan_tail = scanned[-32:]

    def result(self):
        overlap = max(0, len(self.head)+len(self.tail)-self.total_bytes)
        return {'total_bytes': self.total_bytes, 'sha256': self.digest.hexdigest(),
                'capture_truncated': self.total_bytes > self.log_limit,
                'head_hex': self.head.hex(), 'tail_hex': self.tail[overlap:].hex(),
                'final_success_indication': self.final_success_indication,
                'assertion_indications': self.assertion_indications,
                'identities': [{'pid': p, 'tid': t, 'stream': s} for p,t,s in sorted(self.identities)]}


F2_FIELDS = ('abi_version', 'sdk_manifest_sha256', 'newlib_source_sha256',
             'newlib_patch_hashes', 'application_source_sha256', 'application_tests_sha256',
             'elf_hashes', 'argv', 'env', 'cwd', 'fd_setup', 'application_wait_status',
             'declared_exclusions', 'max_resident_pages', 'max_committed_pages', 'resource_ledgers')


def f2_metadata(records):
    """Probe-supplied fields; multipart hex can carry SHA-256 or UTF-8 JSON.

    DATA group=metadata name=<field> part=1 parts=2 encoding=sha256 hex=<half>
    JSON fields use encoding=json and hex-encoded UTF-8, never raw delimiters.
    """
    result = {field: 'unknown' for field in F2_FIELDS}
    chunks = {}
    supplied = set()
    for r in records:
        if r.get('event') != 'DATA' or r.get('group') == 'app':
            continue
        for field in F2_FIELDS:
            if field in r:
                if field in supplied and result[field] != r[field]:
                    raise EvidenceError('contradictory F2 metadata: '+field)
                result[field] = r[field]; supplied.add(field)
        if r.get('group') == 'metadata' and r.get('name') in F2_FIELDS:
            name = r['name']
            try:
                part, parts = int(r['part']), int(r['parts'])
                if not 1 <= part <= parts <= 4096 or not re.fullmatch('[0-9a-fA-F]+', r['hex']):
                    raise ValueError()
                item = chunks.setdefault(name, {'parts': parts, 'encoding': r['encoding'], 'chunks': {}})
                if item['parts'] != parts or item['encoding'] != r['encoding'] or part in item['chunks']:
                    raise ValueError()
                item['chunks'][part] = r['hex']
            except (KeyError, ValueError) as error:
                raise EvidenceError('invalid multipart metadata: '+name) from error
    for name, item in chunks.items():
        if len(item['chunks']) != item['parts']:
            continue
        value = ''.join(item['chunks'][i] for i in range(1, item['parts']+1))
        try:
            if item['encoding'] == 'json':
                value = json.loads(bytes.fromhex(value).decode('utf-8'))
            elif item['encoding'] == 'utf8':
                value = bytes.fromhex(value).decode('utf-8')
            elif item['encoding'] != 'sha256' or not re.fullmatch('[0-9a-fA-F]{64}', value):
                raise ValueError()
        except (ValueError, UnicodeError) as error:
            raise EvidenceError('invalid metadata encoding: '+name) from error
        if name in supplied and result[name] != value:
            raise EvidenceError('contradictory F2 metadata: '+name)
        result[name] = value; supplied.add(name)
    for name in supplied:
        if name.endswith('_sha256') and not re.fullmatch('[0-9a-fA-F]{64}', str(result[name])):
            raise EvidenceError('invalid SHA-256 metadata: '+name)
        if name in ('abi_version','application_wait_status','max_resident_pages','max_committed_pages') and not re.fullmatch('[0-9]+',str(result[name])):
            raise EvidenceError('invalid decimal metadata: '+name)
        if name in ('newlib_patch_hashes','elf_hashes') and (not isinstance(result[name],dict) or not all(isinstance(v,str) and re.fullmatch('[0-9a-fA-F]{64}',v) for v in result[name].values())):
            raise EvidenceError('invalid hash inventory: '+name)
    result['missing_fields'] = [field for field in F2_FIELDS if field not in supplied]
    result['excluded_modes'] = {mode: 'excluded_by_contract' for mode in ('complete', 'internal')}
    return result


class Parser:
    def __init__(self, run_id, probe):
        self.run_id, self.probe = run_id, probe
        self.records = []
        self.seq = 0
        self.started = False
        self.terminal = None
        self.outcome = None
        self.application = ApplicationCapture()

    def feed(self, raw):
        # Line endings are framing, never normalization of marker contents.
        line = raw.removesuffix(b'\n').removesuffix(b'\r')
        if self.terminal and self.terminal['event']=='PANIC' and line.strip():
            raise EvidenceError('output after terminal panic')
        if b'CIUKI_TEST' not in line:
            return
        if not line.startswith(b'CIUKI_TEST ') or len(line) > 240:
            raise EvidenceError('malformed prefix or record exceeds 240 bytes')
        try:
            text = line.decode('ascii')
        except UnicodeDecodeError as e:
            raise EvidenceError('non-ASCII marker') from e
        record = {}
        for token in text.split(' ')[1:]:
            if not re.fullmatch(r'[a-z][a-z0-9_]*=[A-Za-z0-9_:.,/+\-]+', token):
                raise EvidenceError('invalid key=value grammar')
            key, value = token.split('=',1)
            if key in record:
                raise EvidenceError('duplicate key: '+key)
            record[key] = value
        if not {'v','run','seq','probe','event'} <= record.keys():
            raise EvidenceError('missing envelope field')
        if record['v'] != '1' or record['run'] != self.run_id or record['probe'] != self.probe:
            raise EvidenceError('version, run or probe mismatch')
        if record['probe'] not in PROBES or not re.fullmatch('[0-9]{6}', record['seq']):
            raise EvidenceError('unknown probe or invalid sequence')
        seq = int(record['seq'])
        if seq <= self.seq:
            raise EvidenceError('sequence must increase')
        if self.terminal:
            raise EvidenceError('record after terminal event')
        event = record['event']
        if event == 'BEGIN':
            if self.started:
                raise EvidenceError('duplicate BEGIN')
            self.started = True
        elif event == 'NOT_RUN':
            if self.started or record.get('reason') != 'prerequisite_failed' or record.get('after') not in PROBES:
                raise EvidenceError('invalid NOT_RUN record')
        elif not self.started or event not in ('DATA','READY','ARM','END','PANIC','ERROR'):
            raise EvidenceError('unknown event or missing BEGIN')
        if event == 'END' and record.get('status') not in ('PASS','FAIL'):
            raise EvidenceError('END requires PASS or FAIL')
        if event == 'ERROR' and record.get('status') == 'not_run':
            ready = [r for r in self.records if r['event'] == 'READY']
            if record.get('reason') != 'missing_probe' or len(ready) != 1:
                raise EvidenceError('missing probe requires READY then ERROR')
        if event in ('END','PANIC','ERROR','NOT_RUN'):
            self.terminal = record
            self.outcome = 'not_run' if event == 'NOT_RUN' or event == 'ERROR' and record.get('status') == 'not_run' else 'pass' if event == 'END' and record['status'] == 'PASS' else 'fail'
        self.seq = seq
        if event == 'DATA' and record.get('group') == 'app':
            try:
                if record['stream'] not in ('stdout', 'stderr', 'report') or not all(re.fullmatch('[0-9]+',record[k]) for k in ('pid','tid')):
                    raise ValueError()
                if not re.fullmatch('[0-9]+',record['bytes']) or not re.fullmatch('[0-9]+',record['offset']):
                    raise ValueError()
                if not re.fullmatch('(?:[0-9a-fA-F]{2}){1,24}',record['data_hex']):
                    raise ValueError()
                data = bytes.fromhex(record['data_hex'])
                if len(data) != int(record['bytes']) or int(record['offset']) != self.application.total_bytes:
                    raise ValueError()
            except (KeyError, ValueError) as error:
                raise EvidenceError('invalid application frame') from error
            self.application.feed(data, record['tid'], record['stream'], record['pid'])
            return record
        if event == 'DATA' and record.get('group') == 'app_digest':
            if record.get('sha256') != self.application.digest.hexdigest() or record.get('total_bytes') != str(self.application.total_bytes):
                raise EvidenceError('application digest mismatch')
            # Keep the latest cumulative digest, not one record per write.
            self.records[:] = [r for r in self.records if r.get('group') != 'app_digest']
        self.records.append(record)
        return record

    def check(self, expected):
        if self.outcome == 'not_run':
            raise EvidenceError('probe not_run: '+self.terminal.get('reason','prerequisite_failed'))
        if expected['terminal']=='ARM':
            if len([r for r in self.records if r['event']=='ARM'])!=1:
                raise EvidenceError('cut requires exactly one ARM record')
        elif not self.terminal or self.terminal['event'] != expected['terminal']:
            raise EvidenceError('missing or unexpected terminal event')
        if expected['terminal'] == 'END' and self.terminal['status'] != 'PASS':
            raise EvidenceError('kernel reported FAIL')
        for predicate in expected.get('predicates', []):
            matches = [r for r in self.records if all(r.get(k)==str(v) for k,v in predicate.get('where',{}).items())]
            minimum = predicate.get('count', 1)
            if len(matches) < minimum or ('exact_count' in predicate and len(matches)!=predicate['exact_count']):
                raise EvidenceError('missing evidence: '+str(predicate))
            unique=predicate.get('unique')
            if unique and len({r.get(unique) for r in matches})!=len(matches):
                raise EvidenceError('duplicate subcase identity: '+unique)
            if predicate.get('combine',False):
                summary={}
                for record in matches:
                    for key,value in record.items():
                        if key in ('v','run','seq','probe','event'):continue
                        if key in summary and summary[key]!=value:
                            raise EvidenceError('conflicting grouped field: '+key)
                        summary[key]=value
                matches=[summary]
            # Every matching record must meet its predicate: a later good value
            # must not conceal an earlier violation.
            for r in matches:
                for field in predicate.get('required_fields',[]):
                    if field not in r:raise EvidenceError('missing field: '+field)
                for field, rule in predicate.get('fields',{}).items():
                    if field not in r:
                        raise EvidenceError('missing field: '+field)
                    value = r[field]
                    if not isinstance(rule,dict):
                        if value != str(rule):
                            raise EvidenceError('value mismatch: '+field)
                        continue
                    encoding = rule.get('encoding','decimal')
                    pattern = ('-?[0-9]+' if encoding=='signed' else '[0-9]+') if encoding in ('decimal','signed') else '[0-9a-fA-F]{'+str(rule.get('width',8))+'}'
                    if not re.fullmatch(pattern,value):
                        raise EvidenceError('invalid numeric encoding: '+field)
                    number = int(value,10 if encoding in ('decimal','signed') else 16)
                    for op, bound in rule.items():
                        if op in ('encoding','width'): continue
                        if isinstance(bound,str) and bound.startswith('$'):
                            if bound[1:] not in r: raise EvidenceError('missing comparison field')
                            bound = int(r[bound[1:]],10 if encoding in ('decimal','signed') else 16)
                        if op=='eq' and number!=bound or op=='ge' and number<bound or op=='le' and number>bound or op=='ne' and number==bound or op=='mask' and number & bound != bound or op=='clear' and number & bound:
                            raise EvidenceError(f'predicate failed: {field} {op} {bound}')
                        if op not in ('eq','ge','le','ne','mask','clear'):
                            raise EvidenceError('unknown predicate operator')
                for relation in predicate.get('relations',[]):
                    try:
                        left=int(r[relation['left']]);right=int(r[relation['right']])
                        if 'subtract' in relation:right-=int(r[relation['subtract']])
                        right+=relation.get('add',0)
                        right*=relation.get('multiply',1)
                    except (ValueError,KeyError) as e:raise EvidenceError('invalid relation fields') from e
                    op=relation['op']
                    if op=='eq' and left!=right or op=='ge' and left<right or op not in ('eq','ge'):
                        raise EvidenceError('numeric relation failed')
        return True
