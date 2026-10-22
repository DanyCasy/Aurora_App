# VSCode3DS

The entire program is contained in `VSCode3DS.c`. The same file produces both the AuroraOS application and the build service for macOS/Linux, with the mode selected at compile time. No other project-specific source files are required.

`VSCode3DS.bin`, already compiled for AuroraOS, is also included. The source file additionally contains a testing mode for the editing engine.

## Verified Version

AuroraOS repository: [TeamAuroraOS/AuroraOS](https://github.com/TeamAuroraOS/AuroraOS), commit [`8123fd90b606e5a1c44d8a7e474edce83681dcab`](https://github.com/TeamAuroraOS/AuroraOS/tree/8123fd90b606e5a1c44d8a7e474edce83681dcab). Its README identifies **Beta 7 v0.1.6**, and `aurora_app.h` declares **C SDK 1.2**. The previous investigation in this chat referred to an earlier version.

Both the ARM application and the host service were compiled with warnings treated as errors. Testing on a physical console has not been performed: touch input, model compatibility, performance, HOME exit behavior, and Wi-Fi still need to be tested on actual hardware.

## Installing the Application

1. Use AuroraOS Beta 7 with its corresponding runtime and a working SD card.
2. Copy `VSCode3DS.bin` to `SD:/Aurora/Apps/C/`.
3. Launch it from the AuroraOS menu. Its data directory will be `SD:/Aurora/Apps/C/VSCode3DS/`.

The initial project is located at `SD:/Aurora/Apps/C/VSCode3DS/projects/Default/`. You can select another SD card folder using the **Project folder** command. All documents must be saved before switching projects.

Do not launch the IDE by renaming it to `AURORAOS.BIN`. Use the AuroraOS menu, which provides the ARM11 services required for touch input and networking.

## Rebuilding the Application from a Single C File

Install devkitARM and Python 3. Download the verified AuroraOS repository, then run the following command from its root directory:

```sh
python3 sdk/aurcc.py build /path/to/VSCode3DS.c -o VSCode3DS.bin
```

On Windows, the launcher may be `py` instead of `python3`. The host service provided here is intended for macOS/Linux.

## Preparing the Computer for Builds

The computer must have the complete AuroraOS repository, Python 3.10 or later, and devkitARM. Both `arm-none-eabi-gcc` and `arm-none-eabi-objcopy` must be available in the `PATH` inherited by the service.

Compile the service using the same file:

```sh
cc -std=c11 -O2 -Wall -Wextra -DVS3DS_HOST VSCode3DS.c -o vs3ds-host
```

Launch it by specifying the AuroraOS root directory, a token between 16 and 95 characters, and the computer's LAN IP address:

```sh
./vs3ds-host --root /path/to/AuroraOS --token YOUR_LONG_TOKEN --bind 192.168.1.2
```

The IP address is only an example: replace it with your computer's actual address. The default port is `45321`, which can be changed using `--port`. Without `--bind`, the service listens only on `127.0.0.1`, preventing the 3DS from connecting. If the firewall requests permission, allow access on the private network you intend to use.

Communication uses unencrypted TCP for compatibility with AuroraOS. Use a trusted LAN and do not expose this port to the Internet. The token authenticates requests but does not encrypt the token or source files. FNV checksums detect transfer errors but do not provide cryptographic authenticity.

On the 3DS, configure the Wi-Fi connection in the AuroraOS settings. In VSCode3DS, press **START** and configure **Host address**, **Host token**, and **Host port**, then select **Test connection**. The computer must remain powered on with the service running.

## Auric and C Projects

From the menu, you can create `.aur`, `.c`, `.h`, and other text files. Set **Project entry file** to the main source file, such as `main.aur` or `main.c`. Set **Output app name** to define the installed application's name.

- **Auric:** The service compiles the main source file using `compiler.aurc` and produces an `AUR1` container.
- **C:** The service recursively compiles the `.c` files in the project directory using `sdk/aurcc.py`, also using the project directory as an include path, and produces `AURC`.
- Keep independent C programs in separate project directories. Multiple `main` functions within the same project will cause a linker error.
- An `app.icon` file in the project root provides the 32×32 text-based icon displayed in the menu.
- The contents of `assets/` are copied to the installed application's data directory, preserving subdirectories. In C, use `app_dir()` to construct their paths. In Auric, you can use the relative paths supported by its runtime.
- Relative project names must use printable ASCII characters, without hidden or reserved path components. Document contents may use UTF-8.

Press **Y** or select **Build**. The project is saved, transferred in chunks, verified, compiled, and downloaded. You can continue editing buffers, but saving and file-changing operations remain locked during the build to keep the project on disk consistent. Do not modify the SD card from another device while a build is running.

Transfers support chunk retransmission. After transport errors, the client retries and then allows you to resume with **A** or cancel with **B** in the build panel. A single SDK call may temporarily block input and networking; cancellation cannot interrupt that call midway.

Applications are installed in `SD:/Aurora/Apps/` for Auric and `SD:/Aurora/Apps/C/` for C. You can choose between replacement with a `.bak` backup and numbered copies. Launch the result from the AuroraOS Home menu. The program does not invent a direct-launch API that is absent from the public SDK.

## Controls and Interface

| Control | Action |
|---|---|
| D-pad | Moves the cursor; holding it repeats movement |
| A + D-pad | Selects text |
| B | Backspace, with repetition in the editor; cancels in dialogs |
| X | Saves the document |
| Y | Builds the project |
| L / R | Previous / next document |
| START | Opens or closes the complete command list |
| SELECT | Switches keyboard pages |
| SELECT + A | Select all |
| SELECT + Left / Right | Undo / redo |
| SELECT + X / Y | Copy / paste |
| SELECT + Up / Down | Previous / next page |
| Circle Pad | Scrolls the text without moving the cursor |

The system combination **L + Down + SELECT** belongs to AuroraOS.

The upper screen displays the code. The lower screen provides the keyboard, symbols and snippets, file explorer, search, diagnostics, and offline help. **Bottom code / keyboard** enables a lower-screen code view: you can touch a position and double-tap to select a word.

Menu commands include forward/backward search, case sensitivity, whole-word matching, single and global replacement, go to line, word navigation, indentation, comments, line duplication/movement/deletion, selections, and Save As. **Replace current matching selection** replaces a result selected through **Find**.

**Complete identifier** suggests names from the built-in documentation and open documents. The documentation includes the public SDK header and the complete Auric reference from the verified snapshot. You can change the font size, theme, visual word wrapping, tab settings, indentation, automatic pairs, and LF/CRLF line endings.

The file explorer reads pages of 256 entries. Use L/R or the page commands to access additional pages. Entries are sorted within each page. You can also rename open files and directories containing them. Deletion requires textual confirmation, requires affected documents to be closed, and removes only empty directories. FAT does not provide a recycle bin.

## Recovery and Limitations

The session is saved every 15 seconds and during runtime-managed exits, including HOME. Recovery preserves unsaved buffers separately from their original files. The session includes a checksum and a backup. Only the most recent project supports automatic recovery; recent files are retained for that project.

Saving uses a temporary file and a backup. A power interruption can still interrupt a sequence of FAT operations: check `.tmp` and `.bak` files if necessary. An unexpected interruption may result in the loss of changes made after the last successful checkpoint.

| Resource | Limit |
|---|---|
| Open documents | 6 |
| Document / transferred file | 512 KiB; maximum 65,536 lines per document |
| Undo/redo | 64 grouped edits per document, 2 MiB global budget |
| Transferred project | 64 files, 4 MiB total, depth 8 |
| Relative path | 159 bytes |
| Full SD path | Less than 256 bytes, according to the SDK |
| Compiled output | 15 MiB |
| Logs on the 3DS | 64 KiB; full logs on the computer |
| Concurrent builds | 1; maximum 180 seconds per build process |

A CRLF file exceeding 512 KiB after conversion is rejected when saving: use LF or split the document. UTF-8 is preserved and navigated by code point, but the monospace font displays `?` for unsupported glyphs. There is no Unicode IME.

Syntax highlighting and completions are lexical. The compiler running on the computer provides Auric/C semantic errors. A complete language server, Git integration, regular expressions, and an interactive debugger are not included. The IDE cannot compile C or Auric independently on the console.

Logs and working directories remain in `AuroraOS/.vs3ds/<session>/` on the computer. The service does not automatically delete these directories: you can remove old ones while the service is stopped. Restarting the service clears in-memory sessions.

## Tests Performed

- macOS host service compilation with `-Wall -Wextra -Werror`.
- Actual ARM compilation of the single source file using SDK 1.2 with `-Werror`.
- Editing engine tests plus 10,000 randomized cases covering editing, undo, redo, and line indexing, using AddressSanitizer and UndefinedBehaviorSanitizer.
- Command tests with a simulated SDK: brackets, indentation, adjacent replacements/searches, line movement and commenting, saving and recovery, rejection of corrupted sessions, and Auric/C diagnostics.
- TCP protocol testing on localhost with real Auric and C compilers: authentication, fragmented and repeated uploads, checksums, multiple C project headers, empty files, logs, and chunked downloads.
- Rejection of paths outside the project, incomplete builds, modifications to an already-started build, and downloads after errors; cancellation of the compilation process.
- Compilation of the Auric template generated by the editor.

The integrated editing engine test can be executed from the same file:

```sh
cc -std=c11 -g -DVS3DS_TEST VSCode3DS.c -o vs3ds-test
./vs3ds-test
```

Final testing on Old/New 3DS hardware is still required. Frame rates and performance have not been measured on actual hardware.

## Provenance

The source contains the SDK/Auric reference from the specified commit and references to upstream licenses. It is marked `GPL-3.0-or-later`. When distributing the application, also preserve the notices and obligations associated with the linked AuroraOS components, as described in [SDK — Licences](https://github.com/TeamAuroraOS/AuroraOS/blob/8123fd90b606e5a1c44d8a7e474edce83681dcab/sdk/README.md#licences) and the original repository.
