"""Compare production C conversion against independent IANA zoneinfo rules."""
from datetime import datetime, timezone
import subprocess
import sys
from zoneinfo import ZoneInfo

exe=sys.argv[1]
zones=subprocess.check_output([exe,'--zones'],text=True).splitlines()
samples=[]
for number,name in enumerate(zones):
    zone=ZoneInfo(name)
    def offset(epoch):
        return datetime.fromtimestamp(epoch,zone).utcoffset()
    for year in (2024,2026,2027,2028,2032,2037,2099):
        begin=int(datetime(year,1,1,tzinfo=timezone.utc).timestamp())
        finish=int(datetime(year+1,1,1,tzinfo=timezone.utc).timestamp())
        checks={begin,finish-1}
        for month in range(1,13):
            checks.add(int(datetime(year,month,15,13,37,11,tzinfo=timezone.utc).timestamp()))
        # Find actual tzdb offset changes, then test both sides of exact seconds
        # and surrounding quarter-hours (gaps, repeated hours, fractional DST).
        previous=begin
        for current in range(begin+86400,finish,86400):
            if offset(previous)!=offset(current):
                lo,hi=previous,current
                while hi-lo>1:
                    mid=(lo+hi)//2
                    if offset(mid)==offset(lo):lo=mid
                    else:hi=mid
                checks.update((hi-1,hi,hi+1))
                checks.update(range(hi-5400,hi+5401,900))
            previous=current
        for epoch in sorted(checks):
            if 1704067200<=epoch<=4102444799:
                local=datetime.fromtimestamp(epoch,zone)
                samples.append((number,epoch,name,int(local.utcoffset().total_seconds()/60),
                                int(bool(local.dst())),local.strftime('%Y-%m-%d %H:%M:%S')))
payload=''.join(f'{n} {t}\n' for n,t,*_ in samples)
run=subprocess.run([exe,'--oracle'],input=payload,text=True,capture_output=True,check=True)
lines=run.stdout.splitlines()
assert len(lines)==len(samples)
for actual,(number,epoch,name,minutes,dst,calendar) in zip(lines,samples):
    expected=f'{minutes} {dst} {calendar}'
    assert actual==expected,(name,epoch,actual,expected)
print(f'Independent IANA oracle: {len(samples)} conversions across {len(zones)} zones, seven years and exact DST boundaries passed')
