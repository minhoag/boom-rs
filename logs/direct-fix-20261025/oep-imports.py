import frida, pathlib, sys, threading, json, struct

out = pathlib.Path(__file__).parent
sys.stdout = open(out / 'oep-imports.log', 'w', buffering=1)
sys.stderr = sys.stdout
ready = threading.Event()
pid = frida.spawn(sys.argv[1])
session = frida.attach(pid)
script = session.create_script(pathlib.Path(sys.argv[2]).read_text())
def message(m, data):
    print(m, flush=True)
    if m.get('payload', {}).get('event') == 'oep_reached': ready.set()
script.on('message', message)
script.load()
script.exports_sync.setup_oep_tracing('ca.exe', [[0x1000,11567104]])
frida.resume(pid)
if not ready.wait(45):
    frida.kill(pid)
    raise SystemExit('No OEP')
exports = script.exports_sync.enumerate_exported_functions('ca.exe')
raw = script.exports_sync.read_process_memory('0xc0f000',0x850)
slots = []
for a in range(0xc0f7c4,0xc0f848,4):
    target = struct.unpack_from('<I',raw,a-0xc0f000)[0]
    if target:
        module = script.exports_sync.find_module_by_address(hex(target))
        names = [e['name'] for e in exports if int(e['address'],16)==target]
        slots.append(dict(slot=hex(a),target=hex(target),module=module,names=names))
(out/'extra-imports.json').write_text(json.dumps(slots,indent=2))
print(json.dumps(slots),flush=True)
frida.kill(pid)
