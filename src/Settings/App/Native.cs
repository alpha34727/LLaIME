using System.Runtime.InteropServices;
using System.Text.Json;

namespace LLaIME.Settings;

// LLaIME.SettingsNative.dll (src/Settings/Native/SettingsBridge.cpp): the IME's own lexicon,
// user dictionary and engine code. Lists come back as JSON.
internal static class Native
{
    private const string Dll = "LLaIME.SettingsNative.dll";

    private static readonly JsonSerializerOptions Json = new() { PropertyNameCaseInsensitive = true };

    public static string InstallRoot { get; private set; } = "";
    public static InitResult Paths { get; private set; } = new();

    // Where LLaIME is installed: LLAIME_ROOT, the folder above this program
    // (<root>\settings\LLaIME.Settings.exe), or Program Files. LLAIME_DATA replaces
    // %LOCALAPPDATA%\LLaIME (for tests).
    public static void Initialize()
    {
        var root = Environment.GetEnvironmentVariable("LLAIME_ROOT");
        if (string.IsNullOrEmpty(root))
        {
            var parent = Directory.GetParent(AppContext.BaseDirectory.TrimEnd('\\'))?.FullName;
            root = parent != null && Directory.Exists(Path.Combine(parent, "data", "lexicons"))
                ? parent
                : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "LLaIME");
        }
        InstallRoot = root;
        Paths = Parse<InitResult>(LLaime_Init(root, Environment.GetEnvironmentVariable("LLAIME_DATA") ?? "")) ?? new();
    }

    private static T? Parse<T>(string? json) =>
        string.IsNullOrEmpty(json) ? default : JsonSerializer.Deserialize<T>(json, Json);

    // Lexicon layers
    public static List<LexiconInfo> ListLexicons() => Parse<List<LexiconInfo>>(LLaime_ListLexicons()) ?? [];
    public static bool SetLexiconEnabled(string id, bool enabled) => LLaime_SetLexiconEnabled(id, enabled ? 1 : 0) != 0;
    public static ImportResult PreviewImport(string path) => Parse<ImportResult>(LLaime_PreviewImport(path)) ?? new();
    public static ImportResult ImportLexicon(string path, string name, string description, string license) =>
        Parse<ImportResult>(LLaime_ImportLexicon(path, name, description, license)) ?? new();
    public static bool DeleteLexicon(string id) => LLaime_DeleteLexicon(id) != 0;

    // Readings
    public static List<string> GuessReadings(string text) => Parse<List<string>>(LLaime_GuessReadings(text)) ?? [];
    public static bool CheckReading(string reading, string text) => LLaime_CheckReading(reading, text) != 0;

    // User dictionary
    public static List<PhraseInfo> ListPhrases() => Parse<List<PhraseInfo>>(LLaime_ListPhrases()) ?? [];
    public static Result AddPhrase(string reading, string text, string shortcut) =>
        Parse<Result>(LLaime_AddPhrase(reading, text, shortcut)) ?? new();
    public static Result UpdatePhrase(PhraseInfo phrase, string reading, string text, string shortcut) =>
        Parse<Result>(LLaime_UpdatePhrase(phrase.Key, phrase.Text, reading, text, shortcut)) ?? new();
    public static bool RemovePhrase(PhraseInfo phrase) => LLaime_RemovePhrase(phrase.Key, phrase.Text) != 0;
    public static bool MakeManual(PhraseInfo phrase) => LLaime_MakeManual(phrase.Key, phrase.Text) != 0;
    public static List<SelectionInfo> ListSelections() => Parse<List<SelectionInfo>>(LLaime_ListSelections()) ?? [];
    public static bool RemoveSelection(SelectionInfo s) => LLaime_RemoveSelection(s.Key, s.Text, s.Prev) != 0;
    public static int ContextChars
    {
        get => LLaime_GetContextChars();
        set => LLaime_SetContextChars(value);
    }
    public static bool ClearLearning() => LLaime_ClearLearning() != 0;
    public static bool LearningEnabled
    {
        get => LLaime_GetLearningEnabled() != 0;
        set => LLaime_SetLearningEnabled(value ? 1 : 0);
    }
    public static ImportResult ImportPhrases(string path) => Parse<ImportResult>(LLaime_ImportPhrases(path)) ?? new();
    public static int ExportPhrases(string path, bool includeLearned) => LLaime_ExportPhrases(path, includeLearned ? 1 : 0);

    // Engine
    public static EngineStatus EngineStatus() => Parse<EngineStatus>(LLaime_EngineStatus()) ?? new();
    public static List<DeviceInfo>? EngineDevices() => Parse<List<DeviceInfo>>(LLaime_EngineDevices());
    public static bool StartEngine() => LLaime_StartEngine() != 0;
    public static bool ReloadEngine() => LLaime_EngineReload() != 0;
    public static BenchmarkResult Benchmark(int count) => Parse<BenchmarkResult>(LLaime_EngineBenchmark(count)) ?? new();
    public static string ReadEngineSetting(string section, string key) => LLaime_ReadEngineSetting(section, key) ?? "";
    public static bool WriteEngineSetting(string section, string key, string value) =>
        LLaime_WriteEngineSetting(section, key, value) != 0;

    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern string LLaime_Init(string root, string dataDir);
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern string LLaime_ListLexicons();
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern int LLaime_SetLexiconEnabled(string id, int enabled);
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern string LLaime_PreviewImport(string path);
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern string LLaime_ImportLexicon(string path, string name, string description, string license);
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern int LLaime_DeleteLexicon(string id);
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern string LLaime_GuessReadings(string text);
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern int LLaime_CheckReading(string reading, string text);
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern string LLaime_ListPhrases();
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern string LLaime_AddPhrase(string reading, string text, string shortcut);
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern string LLaime_UpdatePhrase(string key, string text, string newReading, string newText, string newShortcut);
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern int LLaime_RemovePhrase(string key, string text);
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern int LLaime_MakeManual(string key, string text);
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern string LLaime_ListSelections();
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern int LLaime_RemoveSelection(string key, string text, string prev);
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern int LLaime_GetContextChars();
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern int LLaime_SetContextChars(int chars);
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern int LLaime_ClearLearning();
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern int LLaime_GetLearningEnabled();
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern int LLaime_SetLearningEnabled(int enabled);
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern string LLaime_ImportPhrases(string path);
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern int LLaime_ExportPhrases(string path, int includeLearned);
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern string LLaime_EngineStatus();
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern string LLaime_EngineDevices();
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern int LLaime_StartEngine();
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern int LLaime_EngineReload();
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern string LLaime_EngineBenchmark(int count);
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern string LLaime_ReadEngineSetting(string section, string key);
    [DllImport(Dll, CharSet = CharSet.Unicode)] private static extern int LLaime_WriteEngineSetting(string section, string key, string value);
}

public class InitResult
{
    public bool Ok { get; set; }
    public string DataDir { get; set; } = "";
    public string EngineIni { get; set; } = "";
    public string BuiltInLexicons { get; set; } = "";
    public string UserLexicons { get; set; } = "";
}

public class Result
{
    public bool Ok { get; set; }
    public string? Error { get; set; }
}

public class LexiconInfo
{
    public string Id { get; set; } = "";
    public string Name { get; set; } = "";
    public string Description { get; set; } = "";
    public string Source { get; set; } = "";
    public string License { get; set; } = "";
    public bool BuiltIn { get; set; }
    public bool Required { get; set; }
    public bool Enabled { get; set; }
    public long Entries { get; set; }
    public string Path { get; set; } = "";
}

public class ImportSample
{
    public string Text { get; set; } = "";
    public string Reading { get; set; } = "";
}

public class ImportResult : Result
{
    public string? Id { get; set; }
    public int Lines { get; set; }
    public int Phrases { get; set; }
    public int Added { get; set; }
    public int Entries { get; set; }
    public int Guessed { get; set; }
    public int UnreadableCount { get; set; }
    public List<string> Unreadable { get; set; } = [];
    public List<ImportSample> Sample { get; set; } = [];
}

public class PhraseInfo
{
    public string Key { get; set; } = "";
    public string Reading { get; set; } = "";
    public string Text { get; set; } = "";
    public int Kind { get; set; }  // 0 manual, 1 learned
    public long Created { get; set; }
    public string Shortcut { get; set; } = "";

    public bool IsLearned => Kind == 1;
    public string ShortcutText => Shortcut.Length > 0 ? "` p " + Shortcut : "";
    public string KindText => Kind == 1 ? "自動成詞" : "手動加入";
    public string CreatedText => DateTimeOffset.FromUnixTimeSeconds(Created).LocalDateTime.ToString("yyyy/MM/dd");
}

public class SelectionInfo
{
    public string Key { get; set; } = "";
    public string Reading { get; set; } = "";
    public string Text { get; set; } = "";
    public string Prev { get; set; } = "";
    public double Weight { get; set; }
    public long Updated { get; set; }

    public string PrevText => Prev.Length == 0 ? "（任何位置）" : Prev;
    public string WeightText => Weight.ToString("0.00");
    public string UpdatedText => DateTimeOffset.FromUnixTimeSeconds(Updated).LocalDateTime.ToString("yyyy/MM/dd HH:mm");
}

public class EngineStatus
{
    public bool Running { get; set; }
    public string State { get; set; } = "";  // loading, ready, failed
    public string Model { get; set; } = "";
    public string Device { get; set; } = "";
    public string Ranker { get; set; } = "";
    public string Error { get; set; } = "";
}

public class DeviceInfo
{
    public string Name { get; set; } = "";
    public string Description { get; set; } = "";
    public ulong Memory { get; set; }
    public bool Gpu { get; set; }
}

public class BenchmarkResult : Result
{
    public int Count { get; set; }
    public double Mean { get; set; }
    public double P50 { get; set; }
    public double P95 { get; set; }
}
