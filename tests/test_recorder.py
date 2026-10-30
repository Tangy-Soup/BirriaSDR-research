"""Run only the injected recorder executable, never a physical USB binary."""
import pathlib,subprocess,os,time,signal,json,sys,tempfile
exe=pathlib.Path(sys.argv[1]).resolve()
root=pathlib.Path(sys.argv[2]) if len(sys.argv)>2 else pathlib.Path(tempfile.mkdtemp(prefix='recorder-host-',dir=exe.parent))
root.mkdir(exist_ok=True)
results=[]
def run(case,sig=None,options=(),count=None):
    name=case+('-'+signal.Signals(sig).name if sig else '')+('-buffered' if '--buffered' in options else '')
    events=root/(name+'.events');out=root/(name+'.cs16')
    assert not events.exists() and not out.exists(),name
    env=dict(os.environ,MOCK_CASE=case,MOCK_EVENTS=str(events),MOCK_SIGNAL=str(sig or signal.SIGINT))
    args=[str(exe),'--frequency','2437000000','--gain','2','8','0',*options,str(out)]
    if count:args+=[str(count)]
    with (root/(name+'.log')).open('w') as log:
        p=subprocess.Popen(args,env=env,stdout=log,stderr=log)
        try:
            if sig and case!='before-read':
                target={'open':'OPEN_BEGIN','setup':'SETUP_BEGIN','callback':'CALLBACK'}.get(case,'READ_BEGIN')
                end=time.monotonic()+3
                while time.monotonic()<end:
                    if events.exists() and target in events.read_text():break
                    assert p.poll() is None,('early exit',name)
                    time.sleep(.002)
                else:raise AssertionError(('missing milestone',name))
                p.send_signal(sig)
            start=time.monotonic();rc=p.wait(timeout=2)
        finally:
            if p.poll() is None:p.kill();p.wait()
    ev=events.read_text().splitlines()
    assert ev[-1]=='CLOSE', (name,ev)
    if case in ('open','setup','watch-fail'):assert 'READ_BEGIN' not in ev,(name,ev)
    else:
        assert 'CLEANUP' in ev and 'WATCH_JOIN' in ev,(name,ev)
        assert ev.index('CLEANUP')<ev.index('WATCH_JOIN')<ev.index('CLOSE'),ev
    if case=='before-read':assert 'CANCEL_STATE' in ev and 'CANCEL_OK' in ev,ev
    expected=1 if case=='watch-fail' or (sig and '--buffered' in options) else 0
    assert rc==expected,(name,rc,expected)
    if sig and '--buffered' in options:
        assert out.stat().st_size==0
        # A cancelled finite read is an interruption, not stale stream data.
        text=(root/(name+'.log')).read_text()
        assert 'Interrupted; buffered recording discarded' in text and 'stream=success' in text,(name,text)
    if count and not sig:
        assert out.stat().st_size==count*4
        # Independent expected byte pattern; --discard=2048 skips one whole block.
        import struct
        block=b''.join(struct.pack('<h',i%31-15) for i in range(4096))
        assert out.read_bytes()==(block*((count+2047)//2048))[:count*4]
    if '--recover' in options:
        gaps=pathlib.Path(str(out)+'.gaps').read_text();assert '2048 4096 1' in gaps,gaps
    row=dict(case=name,returncode=rc,elapsed=time.monotonic()-start,events=ev,bytes=out.stat().st_size if out.exists() else 0)
    results.append(row);print('PASS',name,rc)
for sig in (signal.SIGINT,signal.SIGTERM):
    for case in ('open','setup','before-read','no-callback','callback'):run(case,sig)
    run('no-callback',sig,('--buffered',),2048)
    run('before-read',sig,('--buffered',),2048)
run('watch-fail')
run('finite',options=('--discard','2048'),count=4096)
run('finite',options=('--buffered','--discard','2048'),count=4096)
run('recover',options=('--recover',),count=4096)
(root/'results.json').write_text(json.dumps(results,indent=2))
print('PASS all',len(results),'recorder cases; modeled cleanup only, no physical gain assertion')
