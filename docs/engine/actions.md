# Actions

An action is one step of game logic. Hotspots, triggers and other
actions run lists of actions (`VCActionList`). Each `VCAction` object
holds a data blob and a type byte:

| Field | Content |
|---|---|
| string | Action data, big-endian |
| u8 | Type. Bit 7 set means the data starts with a 12-byte condition. |

The engine reads the action in `0x4138E0 VCAction_read` and byte-swaps
the data for its own use in `0x43B0F0 VCAction_swap_payload`. On disk the
data is big-endian like the rest of the HDB.

The editor code compiled into the engine confirms the data size of each
type: `0x5DB1F0 VCAction_set_arg_count` sets 11, 5, 6, 9, 16, 5,
16 + 5 × arguments and 10 bytes for types 0 to 7, plus 12 when there is a
condition. `0x5D8E50 VCAction_describe` builds the text "If … then …" for
an action; for timers it writes "expires:" and "(periodic)", so the timer
control byte includes a periodic flag.

## Running actions

`fn_DoActionList` (the built-in `DoActionList`) evaluates its arguments
and calls `0x47BD40 vc_queue_action_list`. That function fetches the
action list and the trigger, then wraps each action in a 0x140-byte
queued-action object (`0x40C2B0`, vtable `0x65DBA8`) and pushes it on a
queue. Actions therefore do not run at once; a later step processes the
queue.

Each queued action runs through `0x433060 QueuedAction_execute`, slot 11
of its vtable:

1. Test the condition with `0x43DB70 VCAction_test_condition`. It returns
   true when the action has no condition; otherwise it evaluates both
   operands and compares them with the operation (0 to 7). When the test
   fails, the action is finished without effect.
2. Dispatch on the type:

| Type | Handler |
|---|---|
| 0 | `0x4756D0 act_expression` |
| 1, 7 | `0x479BA0 act_media` (media and sound share one handler) |
| 2 | `0x4285A0 act_timer` |
| 3 | `0x420000 act_enable` |
| 4 | `0x46A0D0 act_set_view` |
| 5 | `0x44E760 act_interface` |
| 6 | `0x472310 act_call_function`: builds the function table on first use, then calls the handler by name |

3. Finish the queued action (`0x432850`).

## Expressions

Conditions and variable statements use the same 11-byte expression (a
condition is padded to 12 bytes):

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | Left operand value |
| 4 | 4 | Right operand value |
| 8 | 1 | Left operand kind |
| 9 | 1 | Right operand kind |
| 10 | 1 | Operation |

Operand kinds:

| Kind | Meaning |
|---|---|
| 0 | A game variable, by ID |
| 1 | A parameter of the current action list: 0–7 integers, 10000–10011 booleans, 20000–20003 bytes |
| 2 | A literal number |
| 3 | A character, for example `'A'`; compared with character variables such as `cSubNode` |
| 4 | A value from the context of the trigger that runs the list |

Operations in conditions: `==`, `!=`, `>`, `<`, `>=`, `<=`, `and`, `or`
(0 to 7). In statements: `=`, `++`, `--`, `+=`, `-=`, `*=`, `/=`, `%=`
(0 to 7).

## Types

After the optional condition, the data depends on the type (low 7 bits):

| Type | Action | Data | Count |
|---|---|---|---|
| 0 | Variable statement | An expression (11 bytes) | 2,354 |
| 1 | Play media | Media object ID (4), selector (1) | 1,859 |
| 2 | Timer | Duration (4), timer number (1), control (1) | 510 |
| 3 | Enable or disable | GAM `VCEnabled` ID (4), value (4, always 0), new state (1: 1 enable, 0 disable) | 5,265 |
| 4 | Go to view | View, node, location and viewpoint IDs (4 each) | 347 |
| 5 | Interface | Interface object ID (4), control (1) | 1,888 |
| 6 | Call a built-in function | Name (15 bytes), argument count (1), argument values (4 each), argument kinds (1 each) | 2,368 |
| 7 | Sound | Sound object ID (4), selector (1), byte (1), two 16-bit values | 63 |

Counts are for the English DVD HDB, which has 14,654 actions. Four of
them have data of an unexpected size.

Example: action list 24226 holds five actions:

```
#24227 media #24228 selector 1
#51985 enable #2587 value 0 control 0
#42470 enable #266 value 0 control 0
#60543 var2046 ++ 0
#67694 var3928 = var605
```

## Built-in functions

Type 6 actions call a built-in engine function by name. The engine fills
a table of 203 names and handlers the first time a function is called,
in `0x472310 act_call_function`. Our IDB names each handler `fn_<Name>`;
`tools/ida/names.txt` lists them all with their addresses.

The game data calls 199 of them (the number in parentheses is the
argument count):

SendEmail(1) ×322, DoActionList(2) ×190, RelocateIFace(5) ×118, SetAIGlow(1) ×112, SwapDisc(5) ×101, DisplayDBox(1) ×99, EnablePDANote(1) ×83, PageTurnSound(0) ×74, ClearAIList(0) ×74, PDAEscape(1) ×66, SetIItemText(7) ×62, SetAIList(2) ×56, MainScroll(1) ×48, StopEnvSounds(0) ×35, SetFLightProps(3) ×35, ScrollTextKill(0) ×32, FadeBack(1) ×31, EndGame(1) ×28, CharTeleport(6) ×27, TextOn(1) ×24, ScrollTextList(5) ×24, DeselectInv(1) ×24, StopAllTimers(0) ×21, OnInvClick(1) ×21, EmailFlushMess(0) ×21, PhotoEnlarge(1) ×20, PhoneButton(3) ×19, IsIItemHeld(2) ×18, SetShanksVar(1) ×16, MarkEmailRead(1) ×16, GetCurrEmail(2) ×16, NukeAllIcons(0) ×15, SaveGameState(0) ×14, PDAInitAddr(0) ×13, INGClear(0) ×13, FadeToBlack(1) ×13, SetLBoxTint(3) ×12, ScrollHeaders(1) ×12, PrefVolume(2) ×12, PrefMusicVol(2) ×12, PrefEffectVol(2) ×12, PrefDialogVol(2) ×12, EnableKitIcon(1) ×12, StartVideoText(4) ×11, PreloadEmIcon(1) ×11, RandGen(2) ×10, FixJumpCut(2) ×10, ClearLBoxTint(0) ×10, AddrSelected(1) ×10, SmAttack(3) ×9, SaveSlotSelect(1) ×9, AddrSelPDA(1) ×9, ScrollAddrs(2) ×8, PrefDefaults(1) ×8, SetIItemFocus(1) ×7, SetIFLAsView(1) ×7, SaveLoadComit(1) ×7, PlayAssetAt(3) ×7, PDAFlipAddr(1) ×7, LoginKeyPress(2) ×7, SetIsNode3Done(0) ×6, ScrollTextInit(1) ×6, SaveKeyPress(2) ×6, EmailInit(1) ×6, DoSmolShootout(0) ×6, GetPlayFBIShot(1) ×5, DisplayAddrs(1) ×5, StopPlayAssets(1) ×4, SelectInv(1) ×4, OnIntInvClick(1) ×4, LoginInit(1) ×4, LoginClick(1) ×4, SmolGoonShot1(0) ×3, SetLanguage(2) ×3, SetIsNode6Done(0) ×3, SetIsNode5Done(0) ×3, SetIsNode4Done(0) ×3, SetIsNode2Done(0) ×3, RollHdCredits(0) ×3, RelocateIFacID(5) ×3, GetPlayBones(1) ×3, ForwardObject(1) ×3, SuicideNote(1) ×2, SmIsWillShot1(1) ×2, SmGoonShoots(3) ×2, SetIsLink3Done(0) ×2, SetIsLink2Done(0) ×2, SaveLoadInit(0) ×2, RollOnEvKit(2) ×2, ReturnMainMenu(0) ×2, RandGenRange(3) ×2, PrepBombTime(2) ×2, PhotoScroll(1) ×2, PhotoInit(0) ×2, PhoneFlipName(2) ×2, NoteFlipDay(1) ×2, JournalFlip(3) ×2, InitSmShootout(3) ×2, InitEmailRead(2) ×2, INGDisplayCook(0) ×2, EmailSend(0) ×2, EmailDelete(1) ×2, DisplayInBox(1) ×2, DispEvIcons(1) ×2, CreateAddrList(1) ×2, BibleFlip(1) ×2, AlienBlast(1) ×2, WillmoreSleeps(0) ×1, TakePhoto(1) ×1, SetStartView(4) ×1, SBaseKeyPress(1) ×1, SBaseInit(0) ×1, ReturnFromHelp(0) ×1, RegXCursorAss(1) ×1, RegViewState(1) ×1, RegViewSpecial(1) ×1, RegUberVars(3) ×1, RegShanksNode6(5) ×1, RegNode6Vars(2) ×1, RegNode5Vars(3) ×1, RegNode4Vars(2) ×1, RegNode3Vars(4) ×1, RegNode2Vars(3) ×1, RegNavBeepAss(1) ×1, RegLink3Vars(2) ×1, RegLink2Vars(2) ×1, RegGunAss(2) ×1, RegFLightAss(1) ×1, RegDlgAss(6) ×1, RegDayNight(1) ×1, RegCurrNode(1) ×1, RegCurrInvVar(2) ×1, RegCookEmail(2) ×1, RegCameraAss(3) ×1, RegBinocAss(1) ×1, RegAIIcon(1) ×1, RefNVisionAss(1) ×1, PwbkOK(1) ×1, PwbkInit(0) ×1, PswdInit(0) ×1, PswdConnect(1) ×1, PrefTranslucnt(1) ×1, PrefTransition(1) ×1, PrefSubliminal(1) ×1, PrefSmoothText(1) ×1, PrefSkipVideo(1) ×1, PrefSkipIntro(1) ×1, PrefSaveOnExit(1) ×1, PrefRollover(1) ×1, PrefLiveEnviro(1) ×1, PrefInventory(1) ×1, PrefInit(0) ×1, PrefHighVideo(1) ×1, PrefFlashBrght(1) ×1, PrefFlashBeam(1) ×1, PrefClosedCap(1) ×1, PrefAutoRewind(1) ×1, PrefAutoRes(1) ×1, PrefAutoDepth(1) ×1, PrefArtIntuit(1) ×1, PrefActionDiff(1) ×1, Pref3DSound(1) ×1, PhotoForward(0) ×1, PhotoDownload(0) ×1, PhotoDelete(0) ×1, PhotoClose(0) ×1, PhoneSend(3) ×1, PhoneMenuInit(1) ×1, PhoneKeyPress(1) ×1, PhoneInit(1) ×1, PhoneDestroy(0) ×1, PhoneClear(1) ×1, OnNVisionClick(1) ×1, OnGunClick(1) ×1, OnFLightClick(1) ×1, OnCameraClick(1) ×1, OnBinocClick(1) ×1, NotePadInit(0) ×1, MSKeyPressed(1) ×1, MediaSearch(0) ×1, MediaInit(0) ×1, MediaDestroy(0) ×1, JournalInit(1) ×1, JournalDestroy(0) ×1, InitAddrList(0) ×1, INGSearch(2) ×1, INGKeyPressed(1) ×1, INGInit(0) ×1, INGDestroy(0) ×1, GetPlayCancer(1) ×1, EvKitIconClick(0) ×1, EnableAllNotes(0) ×1, DrawGunRauch(2) ×1, DoActionListID(2) ×1, ClearIFLExcept(0) ×1, BreakPoint(0) ×1, BibleInit(0) ×1, AudioInit(0) ×1, AddIFLExcept(1) ×1.

The four functions in the table that the data never calls are
`IsoChamberStat`, `RegPreferences`, `RegShanksNode2` and
`RegShanksNode3`.

## Our interpreter

`src/vc/interp.c` runs action lists against the state loaded from
`XFILES.GAM`. It already handles:

- conditions, with all eight comparisons;
- variable statements, on GAM variables and on list locals (parameters);
- enable and disable;
- go to view;
- `DoActionList`, passing its second argument as integer local 0 of the
  called list (still to be confirmed).

It passes media actions and other built-in functions to the game, which
for now only logs them. It runs actions at once instead of queuing them,
and it does not yet read trigger context values (operand kind 4).

```sh
xftool hdb-run XFILES.HDB XFILES.GAM 28618 8   # run the kind-8 triggers of list 28618
XF_TRACE=1 build/xfiles --explore <game> 1236   # explorer with an action trace
```

Example trace when view 1236 is entered:

```
#72533 interface #72504 control 0
#72534 SetIFLAsView(72504)
#84975 var1057 = 1236
```

The last action stores the current view in variable 1057. This supports
the guess that trigger kind 8 means entering.

## Tools

```sh
xftool hdb-actions XFILES.HDB           # count actions by type
xftool hdb-actions XFILES.HDB 24226     # decode an action list
XF_FUNCTIONS=1 xftool hdb-actions XFILES.HDB   # also list every function call
python3 -I tools/re/vc_functions.py path/to/xfiles.exe   # function table
```

## Credits

The action data layout and the operand kinds follow the developer tools
of the X-Files PC enhancement patch; we checked them against the
byte-swap routine in the executable and against all actions in the data.
