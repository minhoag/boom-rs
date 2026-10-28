import frida, pathlib, sys, time, threading

sys.stdout = open(pathlib.Path(__file__).with_name('agent.log'), 'w', buffering=1)
sys.stderr = sys.stdout
print('TRACE START', sys.argv, flush=True)
target = sys.argv[1]
done = threading.Event()
pid = frida.spawn(target)
session = frida.attach(pid)
session.on('detached', lambda *args: (print('DETACHED', args, flush=True), done.set()))
script = session.create_script(pathlib.Path(sys.argv[2]).read_text())
script.on('message', lambda message, data: print(message, flush=True))
script.load()
print('PID', pid, flush=True)
frida.resume(pid)
done.wait(90)
if not done.is_set():
    print('LIVE', script.exports_sync.state(), flush=True)
    session.detach()
