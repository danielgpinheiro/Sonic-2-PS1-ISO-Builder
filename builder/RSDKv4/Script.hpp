#ifndef SCRIPT_H
#define SCRIPT_H

#if RETRO_PLATFORM == RETRO_PS1
// PS1 (2 MB, docs/30 phase 2): sized from the data: GlobalCode + the largest stage (Zone02, without Egg
// Gauntlet) = 98,137 script ints and 7,668 jump ints (Zone11). Script code is stored as int16 slots (96.8% of
// Sonic 2's ints fit): a value outside it stores PS1_SCRIPT_BIG in its slot and goes to a sorted side table
// (Script.cpp), so every code offset, jump table entry and sub pointer stays as in the file. LoadBytecode
// overflowing these is caught (g_ps1ScriptOverflow).
#define SCRIPTCODE_COUNT    (0x18C00)
#define JUMPTABLE_COUNT     (0x2000)
#define PS1_SCRIPTBIG_COUNT (0x1400) // <= 3,119 big constants + ~1,030 slots the scripts write (locals, tables) per stage
#define PS1_SCRIPT_BIG      (-0x8000)
#else
#define SCRIPTCODE_COUNT (0x40000)
#define JUMPTABLE_COUNT  (0x4000)
#endif
#define FUNCTION_COUNT   (0x200)

#define JUMPSTACK_COUNT (0x400)
#define FUNCSTACK_COUNT (0x400)
#define FORSTACK_COUNT  (0x400)

#if RETRO_PLATFORM == RETRO_PS1
#define RETRO_USE_COMPILER (0) // Sonic 2 ships bytecode; the text compiler would only cost RAM
#else
#define RETRO_USE_COMPILER (1)
#endif

struct ScriptPtr {
    int scriptCodePtr;
    int jumpTablePtr;
};

struct ScriptFunction {

    byte access;
#if RETRO_USE_COMPILER
    char name[0x20];
#endif
    ScriptPtr ptr;
};

struct ObjectScript {
    int frameCount;
    int spriteSheetID;
    ScriptPtr eventUpdate;
    ScriptPtr eventDraw;
    ScriptPtr eventStartup;
    int frameListOffset;
    AnimationFile *animFile;
};

struct ScriptEngine {
    int operands[0x10];
    int temp[8];
    int arrayPosition[9];
    int checkResult;
};

enum ScriptSubs { EVENT_MAIN = 0, EVENT_DRAW = 1, EVENT_SETUP = 2 };

extern ObjectScript objectScriptList[OBJECT_COUNT];
extern ScriptFunction scriptFunctionList[FUNCTION_COUNT];

#if RETRO_PLATFORM == RETRO_PS1
int PS1ScriptBigValue(int pos); // Script.cpp: the side table (binary search)
struct PS1ScriptCode {
    short slot[SCRIPTCODE_COUNT];
    inline int operator[](int i) const
    {
        int v = slot[i];
        return v != PS1_SCRIPT_BIG ? v : PS1ScriptBigValue(i);
    }
    // Fields that are always small in valid bytecode (opcodes, operand kinds, array modes, index kinds, variable
    // ids, array position registers): never stored big, read without the check.
    inline int raw(int i) const { return slot[i]; }
};
extern PS1ScriptCode scriptCode; // written by LoadBytecode and PS1ScriptWrite (the compiler is off on PS1)
void PS1ScriptWrite(int pos, int value); // Script.cpp: runtime writes (VAR_LOCAL, SetTableValue)
#else
extern int scriptCode[SCRIPTCODE_COUNT];
#endif
extern int jumpTable[JUMPTABLE_COUNT];

extern int jumpTableStack[JUMPSTACK_COUNT];
extern int functionStack[FUNCSTACK_COUNT];
extern int foreachStack[FORSTACK_COUNT];

extern int scriptCodePos;
extern int scriptCodeOffset;
extern int jumpTablePos;
extern int jumpTableOffset;
extern int jumpTableStackPos;
extern int functionStackPos;
extern int foreachStackPos;

#if RETRO_PLATFORM == RETRO_PS1 && !defined(RETRO_PS1_HOST_TOOL)
// PS1: the VM's hot state (operands, temps, array positions, check result: 136 B) lives in the 1 KB scratchpad
// (0x1F800000, single-cycle; RAM loads cost ~5-7 cycles without a data cache), at its end: 0x1F800378-0x1F8003FF.
// The Sonic CD floor renderer's vertex rows use the first 960 B (ps1/render.cpp): Sonic 2 has no 3D floor layer.
// Zeroed at boot (RSDKv4/main.cpp).
#define PS1_SCRATCH_SCRIPTENG (0x1F800400 - ((sizeof(ScriptEngine) + 7) & ~7))
#define scriptEng             (*reinterpret_cast<ScriptEngine *>(PS1_SCRATCH_SCRIPTENG))
#else
extern ScriptEngine scriptEng;
#endif
#if RETRO_PLATFORM == RETRO_PS1
#define PS1_SCRIPTTEXT_SIZE (0x100) // Sonic 2's longest string operand: 36 chars
extern char scriptText[PS1_SCRIPTTEXT_SIZE];
#else
extern char scriptText[0x4000];
#endif

bool ConvertStringToInteger(const char *text, int *value);

#if RETRO_USE_COMPILER
extern int scriptFunctionCount;
extern char scriptFunctionNames[FUNCTION_COUNT][0x40];

extern int lineID;

void CheckAliasText(char *text);
void CheckStaticText(char *text);
bool CheckTableText(char *text);
void ConvertArithmaticSyntax(char *text);
void ConvertConditionalStatement(char *text);
bool ConvertSwitchStatement(char *text);
void ConvertFunctionText(char *text);
void CheckCaseNumber(char *text);
bool ReadSwitchCase(char *text);
void ReadTableValues(char *text);
void AppendIntegerToString(char *text, int value);
void AppendIntegerToStringW(ushort *text, int value);
void CopyAliasStr(char *dest, char *text, bool arrayIndex);
bool CheckOpcodeType(char *text); // Never actually used

void ParseScriptFile(char *scriptName, int scriptID);
#endif
void LoadBytecode(int stageListID, int scriptID);

void ProcessScript(int scriptCodeStart, int jumpTableStart, byte scriptEvent);

void ClearScriptData(void);

#endif // !SCRIPT_H