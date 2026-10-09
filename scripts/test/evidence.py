"""Strict raw F0/F1 evidence parser and declarative DATA predicates."""
import re
from loader_model import PROBES


class EvidenceError(ValueError):
    pass


class Parser:
    def __init__(self, run_id, probe):
        self.run_id, self.probe = run_id, probe
        self.records = []
        self.seq = 0
        self.started = False
        self.terminal = None

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
        elif not self.started or event not in ('DATA','READY','ARM','END','PANIC','ERROR'):
            raise EvidenceError('unknown event or missing BEGIN')
        if event == 'END' and record.get('status') not in ('PASS','FAIL'):
            raise EvidenceError('END requires PASS or FAIL')
        if event in ('END','PANIC','ERROR'):
            self.terminal = record
        self.seq = seq
        self.records.append(record)

    def check(self, expected):
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
                        left=int(r[relation['left']]);right=int(r[relation['right']])*relation.get('multiply',1)
                    except (ValueError,KeyError) as e:raise EvidenceError('invalid relation fields') from e
                    if relation['op']!='eq' or left!=right:raise EvidenceError('numeric relation failed')
        return True
