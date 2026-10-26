"""Analyze ordered HID delivery gaps; USB report timers are not BT timestamps."""
from collections import Counter
import argparse
import json
from pathlib import Path
import statistics


def patterns(samples):
    times=[s["elapsed_ms"] for s in samples]
    gaps=[b-a for a,b in zip(times,times[1:])]
    if len(gaps)<2:
        return {"error":"Need at least three ordered samples"}
    # 12 ms separates nominal 8 and 16 ms deliveries, allowing host jitter.
    long=[i for i,g in enumerate(gaps) if g>12]
    slots=[round(g/8) for g in gaps]
    def hist(values):
        return dict(sorted(Counter(values).items()))
    mean=statistics.mean(gaps)
    centered=[g-mean for g in gaps]
    correlations=[]
    for lag in range(1,min(129,len(gaps)//4)):
        a=centered[:-lag]; b=centered[lag:]
        denom=(sum(x*x for x in a)*sum(x*x for x in b))**.5
        if denom:
            correlations.append({"lag_reports":lag,"correlation":sum(x*y for x,y in zip(a,b))/denom})
    tails=[]
    for i,g in enumerate(gaps):
        if g>20:
            tails.append({"report_index":i+1,"elapsed_ms":times[i+1],"gap_ms":g,
                          "neighbor_gaps_ms":gaps[max(0,i-3):i+4]})
    return {
        "clock":"Windows read completion; receiver-generated USB timer is not a BT timestamp",
        "long_threshold_ms":12,"long_gap_count":len(long),
        "long_gap_spacing_reports":hist(b-a for a,b in zip(long,long[1:])),
        "nominal_8ms_slot_counts":hist(slots),
        "slot_transitions":dict(sorted(Counter(f"{a}->{b}" for a,b in zip(slots,slots[1:])).items())),
        "within_0_75ms_of_8ms_multiple":sum(abs(g-8*n)<=.75 for g,n in zip(gaps,slots))/len(gaps),
        "largest_absolute_autocorrelations":sorted(correlations,key=lambda r:abs(r["correlation"]),reverse=True)[:10],
        "gaps_over_20ms":tails,
    }


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture",type=Path)
    args=parser.parse_args()
    data=json.loads(args.capture.read_text())
    for i,phase in enumerate(data["phases"]):
        if "samples" not in phase:
            raise SystemExit("This capture saved aggregates only; packet ordering cannot be reconstructed.")
        print(json.dumps({"phase":i,**patterns(phase["samples"])},indent=2))
