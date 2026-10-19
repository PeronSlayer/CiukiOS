"""Reconcile UART and disk records before making a zero-loss/path claim.

No guest execution. Inputs must belong to one run and one immutable image.
The global CIUKI_TEST sequence includes disk-only records. Identical records
may occur once in each sink; conflicting/repeated records in one sink fail.
SPDX-License-Identifier: GPL-2.0-only
"""
import argparse
import json
from pathlib import Path
import re
from conformance import tags, digest


def fields(line):
    pairs=re.findall(r"([a-zA-Z_][a-zA-Z_0-9]*)=([^ ]+)",line)
    result=dict(pairs)
    if len(pairs)!=len(result): raise ValueError("duplicate trace field")
    return result


def reconcile(serial, disk, run):
    merged={}
    for sink_index,sink in enumerate((serial,disk)):
        seen=set();previous=0
        for line in sink.splitlines():
            if not line.startswith("CIUKI_TEST "): continue
            row=fields(line)
            # Disk boot history can contain earlier boots; never merge them.
            if row.get("run")!=run:
                if sink_index==0: raise ValueError("stale serial run")
                continue
            seq=int(row["seq"])
            if (sink_index==0 and seq<=previous) or seq in seen: raise ValueError("reordered/duplicate sink record")
            seen.add(seq);previous=seq
            if seq in merged and merged[seq][0]!=line: raise ValueError("conflicting serial/disk record")
            if row.get("event")=="ERROR" or row.get("status")=="FAIL": raise ValueError("failed trace producer")
            for key in ("dropped","trace_dropped","cleanup_error","lost"):
                if int(row.get(key,"0")): raise ValueError("declared trace loss: "+key)
            merged[seq]=(line,row)
    ledgers=[(seq,row) for seq,(_,row) in merged.items() if row.get("group")=="record_ledger"]
    if not ledgers: raise ValueError("missing closing record ledger")
    # Concurrent title VMs can each close a watermark. Check every witness,
    # then reconcile through the last one, which covers the complete stream.
    start=None
    for witness,ledger in sorted(ledgers):
        first,last,total=(int(ledger[k]) for k in ("first","last","records"))
        if first<1 or last!=witness-1 or total!=last-first+1 or last<first or total>1000000 or (start is not None and first!=start):
            raise ValueError("invalid record ledger watermark")
        start=first
    missing=set(range(first,last+1))-merged.keys()
    if missing: raise ValueError("unreconciled serial/disk sequence gaps: "+str(sorted(missing)[:16]))
    return [merged[s][1] for s in sorted(merged) if s<=last]


PART_KEYS={1:{"call","edge","psp","origin","vector"},
           2:{"ax","bx","cx","dx","ds","es","si","di"},
           3:{"bp","flags","cs","ip","ss","sp","bytes"},
           4:{"monitor_us","wall_us","dropped"}}
HEX={"ax","bx","cx","dx","ds","es","si","di","bp","flags","cs","ip","ss","sp","psp","vector"}


def calls(records):
    states={};completed=[]
    for row in records:
        if row.get("group") not in ("dos_api","dos_trace_end"): continue
        key=(int(row["vm"]),int(row["gen"]))
        state=states.setdefault(key,dict(row=0,part=1,parts={},active={},ended=False))
        if state["ended"]: raise ValueError("trace after VM ledger")
        if row["group"]=="dos_trace_end":
            if state["part"]!=1 or int(row["records"])!=state["row"] or int(row["pending"]) or state["active"]:
                raise ValueError("partial trace/call or nonzero teardown ledger")
            state["ended"]=True;continue
        index,part=int(row["row"]),int(row["part"])
        if index!=state["row"]+1 or part!=state["part"]: raise ValueError("missing/reordered trace fragment")
        expected=PART_KEYS[part]
        if not expected<=row.keys(): raise ValueError("trace register fragment missing fields")
        state["parts"].update({k:int(row[k],16 if k in HEX else 10) for k in expected})
        if part<4: state["part"]+=1;continue
        event=state["parts"];event.update(vm=key[0],generation=key[1],record=index,wire_sequence=int(row["seq"]))
        if event["dropped"]: raise ValueError("declared DOS trace loss")
        state.update(row=index,part=1,parts={})
        call,edge=event["call"],event["edge"]
        if edge==0:
            if call in state["active"]: raise ValueError("duplicate entry")
            state["active"][call]=event
        elif edge in (1,2,3):
            if call not in state["active"]: raise ValueError("return/termination without entry")
            entry=state["active"].pop(call)
            if any(entry[k]!=event[k] for k in ("psp","origin","vector")): raise ValueError("call identity changed")
            completed.append(dict(entry=entry,result=event,termination=edge in (2,3),cancelled=edge==3))
        else: raise ValueError("unknown trace edge")
    if not states or not all(state["ended"] for state in states.values()): raise ValueError("missing VM trace ledger")
    return completed


def inventory(rows, coverage=None, approved_errors=()):
    if coverage is None:
        coverage={}
        for tag,case in tags().items():
            for function in case.functions: coverage.setdefault(function,[]).append(tag)
    result=[]
    for row in rows:
        entry,output=row["entry"],row["result"]
        vector,ax=entry["vector"],entry["ax"]
        selector=f"{vector:02X}:{ax:04X}"
        possible=(selector,f"{vector:02X}:{ax>>8:02X}",f"{vector:02X}:*")
        tests=sorted({test for name in possible for test in coverage.get(name,[])})
        if not tests: raise ValueError("unmapped observed selector: "+selector)
        failed=bool(output["flags"]&1) and not row["termination"]
        if vector==0x21 and failed and output["ax"]==1: raise ValueError("unsupported DOS function on observed path")
        if failed and (selector,output["ax"]) not in approved_errors:
            raise ValueError("unapproved failed call: "+selector+f" AX={output['ax']:04X}")
        result.append(dict(selector=selector,registers={k:entry[k] for k in HEX if k in entry},
                           result=output,tests=tests,termination=row["termination"],evidence="observed"))
    return result


def title_paths(records, evidence, rows):
    """Lead supplies measured stage boundaries; names alone cannot cover calls."""
    if evidence.get("title") not in ("doom","quake","wolf3d"): raise ValueError("unknown pinned title")
    required=["loader","start","menu","demo","play","save","load","config-write","config-reopen","exit"]
    if [r.get("stage") for r in evidence.get("paths",[])]!=required: raise ValueError("incomplete title paths")
    if any(type(evidence.get(k)) is not int or evidence[k]<=0 for k in ("vm","generation")):
        raise ValueError("missing title VM identity")
    rows=[r for r in rows if (r["entry"].get("vm"),r["entry"].get("generation"))==(evidence["vm"],evidence["generation"])]
    available={int(row["seq"]) for row in records};last=0
    for path in evidence["paths"]:
        begin,end=path["first"],path["last"]
        if not last<begin<=end or begin not in available or end not in available: raise ValueError("invalid title trace range")
        if not any(begin<=r["entry"]["wire_sequence"]<=end for r in rows): raise ValueError("untraced title stage")
        if any(r.get("cancelled") and begin<=r["entry"]["wire_sequence"]<=end for r in rows):
            raise ValueError("cancelled call inside accepted title stage")
        last=end
    return evidence["paths"]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--serial",type=Path,required=True);p.add_argument("--disk",type=Path,required=True)
    p.add_argument("--run",required=True);p.add_argument("--evidence",type=Path,required=True)
    p.add_argument("--image",type=Path,required=True)
    p.add_argument("--out",type=Path,required=True)
    args=p.parse_args();evidence=json.loads(args.evidence.read_text())
    if not re.fullmatch(r"[0-9a-f]{8}",args.run): raise ValueError("invalid run")
    if any(not re.fullmatch(r"[0-9a-f]{64}",evidence.get(key,"")) for key in ("image_sha256","source_sha256","executable_sha256")):
        raise ValueError("missing image/source/executable identities")
    if digest(args.image)!=evidence["image_sha256"]: raise ValueError("title image hash mismatch")
    audit=json.loads((Path(__file__).resolve().parents[3]/"docs/validation/f3-48-static-candidates.json").read_text())
    path={"doom":"/DOOM/DOOM.EXE","quake":"/QUAKE/QUAKE.EXE","wolf3d":"/WOLF3D/WOLF3D.EXE"}.get(evidence.get("title"))
    if not any(r["path"]==path and r["sha256"]==evidence["executable_sha256"] for r in audit["files"]):
        raise ValueError("title executable hash differs from pinned original")
    records=reconcile(args.serial.read_text(),args.disk.read_text(),args.run)
    observed=calls(records)
    errors=[]
    for row in evidence.get("approved_errors",[]):
        if not row.get("approval") or not row.get("reason"): raise ValueError("unapproved implementation difference")
        errors.append((row["selector"],row["ax"]))
    paths=title_paths(records,evidence,observed)
    selected=[r for r in observed if (r["entry"]["vm"],r["entry"]["generation"])==(evidence["vm"],evidence["generation"])]
    result=dict(evidence=evidence,serial_sha256=digest(args.serial),disk_sha256=digest(args.disk),paths=paths,
                calls=inventory(selected,evidence.get("coverage"),errors))
    args.out.write_text(json.dumps(result,indent=2)+"\n")
    return 0


if __name__=="__main__": main()
