# Lessons from fixing the legacy client — 2026-10-25

Scope: strip the Themida protection layer and patch the legacy VN client `ca.exe` so it runs under Lutris/Wine and reaches the local lobby. This is not KR fidelity evidence, and it is not acceptance of the Rust rewrite.

Repo baseline: `7f5ba3fde369dc9de1805646ce080c754fdbd9b5`. The client fix lives in [boomlaunch.c](boomlaunch.c). This note records the investigation the project owner asked for. It does not replace the gameplay-rule source.

## 1. Result and limits

- `vn_runtime` / `confirmed`: the project owner confirmed login into the lobby. The patched build no longer shows a black screen when launched through the launcher.
- `vn_runtime` / `confirmed`: after the patches were written into the file, the installed `ca.exe` creates an 800×600 `Boom` window with the Lutris prefix and runner. `boomlaunch.exe` is not required.
- `unknown`: login through to the lobby has not been confirmed again on the direct-run build. A full match, and two clients at once, are not confirmed.
- Stripping the protection does not restore the original source. There is no basis for treating a decompiled `ca.c` as source that can be rebuilt into an equivalent client.

## 2. No window: a missing import tail, not a render bug

`vn_static` + `vn_runtime` / `confirmed` on the build that was investigated:

- The old IAT from the Python pipeline stopped at `0xC0F7FC`. Slots after that still held runtime addresses from an earlier Wine process. Those addresses are not safe to reuse on another run.
- The client failed when it called `OleInitialize` through slot `0xC0F838`, before it created a window and before the new init log.
- Resolve import names at the OEP with Frida, then rebuild the imports by DLL and name. Do not freeze a runtime address.
- Added 11 `nmco3.dll` imports at `0xC0F800`–`0xC0F828`, and 6 COM/OLE imports at `0xC0F830`–`0xC0F844`: `CoInitialize`, `OleUninitialize`, `OleInitialize`, `CoUninitialize`, `CoCreateInstance`, `CoCreateGuid`.
- `boomlaunch.c --repair-import-tail <file>` uses free space in `.SCY`. It updates the header only after the extra imports are written successfully.

Lesson: a dump that has the OEP and most imports correct is still not a standalone executable. Check the whole IAT tail, and keep VA, RVA, and file offset distinct.

## 3. Unlicense: a runtime condition that mattered here

In this investigation, turning DEP on with `SetProcessDEPPolicy(1)` in the target let Python `ergrelet/unlicense` find OEP `0x9E5EFE`. That is an observation for this environment. It is not a universal requirement of every Themida build.

The target needs its assets beside it and the correct working directory. Reusing a dump together with an IAT or devirtualization artifact that already worked was more effective than running the whole unpack again. Do not change the unpacker or add a fallback unless the user agrees.

## 4. Black screen: a Wine media path aborted

`vn_runtime` / `confirmed`: the `AMMultiMediaStream` init path calls an export Wine does not implement: `winedmo.dll.winedmo_create_color_converter`. This is not simply a missing DLL. The DLL in the prefix and the Proton copy were compared and are the same version.

`design` / `design`: patch the call at VA `0x480332`:

```text
E8 4A CC FF FF  ->  58 83 C8 FF 90
```

The patch pops one stdcall argument off the stack, returns `EAX = -1`, and takes the error branch that already exists. It does not disable media DLLs in bulk. This skips the broken media path. It is not a full codec or video fix. A feature that needs that path may not work.

After the fix: the launcher reported 27/27 patches. The window appeared. `CALog` contains `EndOfInit!! exitcode=0`. The server received login traffic on 3838/4848. The user confirmed entry into the lobby.

## 5. Writing the patches into the existing ca.exe

The patch source is the table currently enabled in `boomlaunch.c`, not the windowed experiment at `0xA56791`.

| Group | Sites | What it does |
|---|---:|---|
| Code | 4 | HackShield/BlackCipher `0x8E701D`, PatchServer `0x8E56ED`, media `0x480332`, Wine/autoupdate `0x69E636` |
| Calls that return success | 13 | Calls in the patch table, written as `B0 01 90 90 90` |
| IP | 10 | Server address becomes `127.0.0.1`, padded with NUL inside the old string region |
| Total | 27 | 24 sites were already correct. The last write changed only 3 more sites |

Do not confuse 27 confirmed sites with 27 new changes. The live table has 13 calls that return success, not the 16 mentioned in an older description.

The direct write used a one-shot Python script that read the C table: check the input hash, map VA to file offset from the PE, check the old bytes or the already-patched bytes, keep the size, back up, write a staging file, replace, then read back and compare. The launcher has no mode that writes the whole patch set into the file. Do not describe this one-shot step as a CLI feature that already exists.

## 6. Configuration that is kept

- Client: `/mnt/Games/boom-online/drive_c/Program Files (x86)/VNG/Boom/ca.exe`.
- Lutris: `/home/wumps/.local/share/lutris/games/boom-online-1792601869.yml`. It points straight at `ca.exe`. The launcher IP argument is gone.
- Prefix: `/mnt/Games/boom-online-umu10`. Runner: `UMU-Proton-10.0-4`.
- Working directory: the `Boom` folder that holds the client and the assets. The launch chain that was used: `steam-run` → `umu-run` → Proton.
- `boomlaunch.exe` is no longer required for a direct launch, but keep it for rollback and investigation. `BOOM_CAPTURE` is opt-in. `BOOM_EXPERIMENT_WINDOWED` is an unconfirmed experiment. Do not bake it into the client.
- The IP is fixed at `127.0.0.1`. Changing the server means patching again. Do not write a string longer than the allocated region. Addresses and bytes in this note apply only to the build whose hash is below.

SHA-256:

```text
Original client, backed up:
bce8644d4e783d1354ab4fc880e1ab9a4ab3325327028f37d99e5439e25f4bbd
Client with imports repaired, before the final patch bake:
ebb97fd4c4bd17fb97ab02aa5b28dbe8f7ded83bf00d01fccc9690aaffb9a520
Client that runs directly now:
fbd6b6eef3a5116d52542d5eade0073034c12e57ee741d487f3111d222dc170a
Launcher that was deployed:
41c2bac0bfe92b1ddc6564975d39081a277a5cfe490dd042b14b1c1e059d54fc
```

## 7. Evidence and rollback

Paths below are from the repo root. They stay as written so existing references do not break.

- `logs/direct-fix-20261025/extra-imports.json`: slot, export name, and the DLL resolved at runtime.
- `logs/direct-fix-20261025/boomlaunch.log`: patch result through the launcher.
- `logs/direct-fix-20261025/server.log`, `server-console.log`: local server traffic.
- `logs/direct-fix-20261025/deployment.json`: the deploy from before the final bake. The Lutris note in that file is historical state.
- `logs/direct-ca-20261025T160509/patch-result.json`: VA, file offset, bytes before and after each patch, and the hash of the direct-run build.
- `logs/direct-ca-20261025T160509/direct-run.log`, `windows-before.json`, `windows-after.json`, `run.json`: evidence of the direct launch.
- `logs/direct-ca-20261025T160509/ca.exe.before-direct-patch.bak` and `lutris.before-direct-patch.yml.bak`: roll the client and config pair back to launcher-based launch, from before the final bake.
- `logs/direct-fix-20261025/ca.exe.before-install.bak`, `boomlaunch.exe.before-install.bak`: the install from before that deploy. That client still has protection. Do not confuse it with a rollback to an unpacked build that already runs.
- `logs/unlicense-20261025T065707Z/`: the dump and intermediate artifacts. Keep them so the steps can be repeated. Do not treat every candidate as a build that runs.

Rollback must close the client, save the current state, then restore the matching client and config pair. Do not put the original client back while leaving a config that belongs to the unpacked build.

## 8. Cleaning up the session, and what to avoid

- Sent SIGTERM to the direct-run client PID `114758` and the test server PID `103974`, after checking the command line. Did not use `killall`, and did not shut down the whole wineserver.
- Keep the installed client, the launcher, the source, the dump, the logs, and the backups. Do not bulk-delete under a vague "everything". Do not reset `.gitignore` or `server.toml` changes, and do not edit the launcher beyond what was asked.
- The test server was stopped. A local login needs `tools/fakeserver.py` started again. A stopped server is not a new client bug.
- Did not run tests, lint, or type-check while writing this note. Rechecked the installed client hash and the Lutris config. Did not launch the game again.
- Not committed, pushed, or merged. Do not raise Rust feature progress from a legacy-client fix.

Process lesson: debug the first failure directly, one hypothesis at a time, save the evidence, then deploy. Do not hand the whole pipeline back to a subagent to repeat. A window appearing, or a server that sees traffic, does not replace the user's confirmation of login and of a match.
