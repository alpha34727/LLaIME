using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;
using Windows.Storage.Pickers;

namespace LLaIME.Settings.Pages;

// engine.ini [model] ranker / path / device / threads and [laya] path; applied with a reload
// of the engine (no restart, typing keeps working on the lexicon meanwhile).
public sealed partial class ModelPage : Page
{
    private readonly List<string> modelPaths_ = [];
    private readonly List<string> deviceNames_ = [];
    private string layaDir_ = "";
    private bool loading_;
    private DispatcherTimer? statusTimer_;

    public ModelPage()
    {
        InitializeComponent();
    }

    protected override async void OnNavigatedTo(NavigationEventArgs e)
    {
        loading_ = true;
        Select(RankerBox, Native.ReadEngineSetting("model", "ranker") is { Length: > 0 } r ? r : "auto");
        LoadModels(Native.ReadEngineSetting("model", "path"));
        layaDir_ = Native.ReadEngineSetting("laya", "path");
        LayaCard.Description = layaDir_.Length > 0 ? layaDir_ : "尚未設定";
        ThreadsBox.Value = int.TryParse(Native.ReadEngineSetting("model", "threads"), out var t) ? t : 0;
        DeviceBox.Items.Clear();
        DeviceBox.Items.Add("自動");
        deviceNames_.Clear();
        deviceNames_.Add("auto");
        DeviceBox.SelectedIndex = 0;
        loading_ = false;

        statusTimer_ = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        statusTimer_.Tick += async (_, _) => await RefreshStatusAsync();
        statusTimer_.Start();
        await RefreshStatusAsync();
        await LoadDevicesAsync(Native.ReadEngineSetting("model", "device"));
    }

    protected override void OnNavigatedFrom(NavigationEventArgs e) => statusTimer_?.Stop();

    private static void Select(ComboBox box, string tag)
    {
        foreach (var item in box.Items)
        {
            if (item is ComboBoxItem { Tag: string t } c && t == tag) box.SelectedItem = c;
        }
        if (box.SelectedItem == null) box.SelectedIndex = 0;
    }

    // GGUF files next to the current model, in its parent folder (and their subfolders), and in
    // <root>\models and %LOCALAPPDATA%\LLaIME\models.
    private void LoadModels(string current)
    {
        var dirs = new List<string>();
        if (current.Length > 0 && Path.GetDirectoryName(current) is { } dir)
        {
            dirs.Add(dir);
            if (Path.GetDirectoryName(dir) is { } parent) dirs.Add(parent);
        }
        dirs.Add(Path.Combine(Native.InstallRoot, "models"));
        dirs.Add(Path.Combine(Native.Paths.DataDir, "models"));
        var found = new SortedSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var d in dirs.Where(Directory.Exists))
        {
            foreach (var f in Directory.EnumerateFiles(d, "*.gguf")) found.Add(f);
            foreach (var sub in Directory.EnumerateDirectories(d))
            {
                foreach (var f in Directory.EnumerateFiles(sub, "*.gguf")) found.Add(f);
            }
        }
        if (current.Length > 0) found.Add(current);
        modelPaths_.Clear();
        ModelBox.Items.Clear();
        foreach (var path in found)
        {
            modelPaths_.Add(path);
            ModelBox.Items.Add(ModelLabel(path));
        }
        ModelBox.SelectedIndex = modelPaths_.FindIndex(p => string.Equals(p, current, StringComparison.OrdinalIgnoreCase));
        UpdateModelPath();
    }

    private static string ModelLabel(string path)
    {
        var name = Path.GetFileNameWithoutExtension(path);
        var size = File.Exists(path) ? $"{new FileInfo(path).Length / 1e9:0.0} GB" : "找不到檔案";
        var tw = name.Contains("-tw", StringComparison.OrdinalIgnoreCase) ? "台灣 LoRA · " : "";
        return $"{name}（{tw}{size}）";
    }

    private void UpdateModelPath()
    {
        ModelPathCard.Description = ModelBox.SelectedIndex >= 0 ? modelPaths_[ModelBox.SelectedIndex] : "尚未選擇模型";
    }

    private async Task LoadDevicesAsync(string current)
    {
        var devices = await Task.Run(Native.EngineDevices);
        if (devices == null) return;
        loading_ = true;
        foreach (var d in devices)
        {
            var memory = d.Memory > 0 ? $"，{d.Memory / (1024.0 * 1024 * 1024):0.#} GB" : "";
            var kind = d.Name.StartsWith("CUDA") ? "NVIDIA CUDA" : d.Name.StartsWith("Vulkan") ? "Vulkan"
                : d.Name == "CPU" ? "CPU" : d.Name;
            DeviceBox.Items.Add(d.Name == "CPU" ? $"CPU（{d.Description}）" : $"{kind}：{d.Description}{memory}");
            deviceNames_.Add(d.Name);
        }
        if (!devices.Any(d => d.Name == "CPU"))
        {
            DeviceBox.Items.Add("CPU");
            deviceNames_.Add("CPU");
        }
        var index = deviceNames_.FindIndex(n => string.Equals(n, current, StringComparison.OrdinalIgnoreCase));
        if (index < 0 && current.Length > 0 && current != "auto")
        {
            DeviceBox.Items.Add($"{current}（目前不可用）");
            deviceNames_.Add(current);
            index = deviceNames_.Count - 1;
        }
        DeviceBox.SelectedIndex = Math.Max(0, index);
        loading_ = false;
    }

    private async Task RefreshStatusAsync()
    {
        var status = await Task.Run(Native.EngineStatus);
        StartEngineButton.Visibility = status.Running ? Visibility.Collapsed : Visibility.Visible;
        BenchmarkButton.IsEnabled = status.Running && status.State == "ready";
        if (!status.Running)
        {
            StatusBar.Severity = InfoBarSeverity.Warning;
            StatusBar.Title = "選字引擎沒有在執行";
            StatusBar.Message = "輸入法會在下次打字時自動啟動它，也可以現在啟動。";
        }
        else if (status.State == "ready")
        {
            StatusBar.Severity = InfoBarSeverity.Success;
            StatusBar.Title = status.Ranker == "laya" ? "已載入 CPU 模式模型" : "已載入語言模型";
            var device = status.Device is "" or "auto" ? "自動選擇裝置" : status.Device;
            StatusBar.Message = $"{status.Model}（{device}）";
        }
        else if (status.State == "loading")
        {
            StatusBar.Severity = InfoBarSeverity.Informational;
            StatusBar.Title = "正在載入模型…";
            StatusBar.Message = "載入期間輸入法以詞庫選字。";
        }
        else
        {
            StatusBar.Severity = InfoBarSeverity.Error;
            StatusBar.Title = "模型無法載入，目前只用詞庫選字";
            StatusBar.Message = status.Error.Length > 0 ? status.Error : "請檢查模型檔案與加速裝置設定。";
        }
    }

    private void Setting_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (ReferenceEquals(sender, ModelBox)) UpdateModelPath();
        if (!loading_) ApplyButton.IsEnabled = true;
    }

    private void Threads_Changed(NumberBox sender, NumberBoxValueChangedEventArgs args)
    {
        if (double.IsNaN(sender.Value)) sender.Value = 0;
        if (!loading_) ApplyButton.IsEnabled = true;
    }

    private async void BrowseModel_Click(object sender, RoutedEventArgs e)
    {
        var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.ComputerFolder };
        picker.FileTypeFilter.Add(".gguf");
        WinRT.Interop.InitializeWithWindow.Initialize(picker, App.Window!.Handle);
        if (await picker.PickSingleFileAsync() is not { } file) return;
        var index = modelPaths_.FindIndex(p => string.Equals(p, file.Path, StringComparison.OrdinalIgnoreCase));
        if (index < 0)
        {
            modelPaths_.Add(file.Path);
            ModelBox.Items.Add(ModelLabel(file.Path));
            index = modelPaths_.Count - 1;
        }
        ModelBox.SelectedIndex = index;
    }

    private async void BrowseLaya_Click(object sender, RoutedEventArgs e)
    {
        var picker = new FolderPicker { SuggestedStartLocation = PickerLocationId.ComputerFolder };
        picker.FileTypeFilter.Add("*");
        WinRT.Interop.InitializeWithWindow.Initialize(picker, App.Window!.Handle);
        if (await picker.PickSingleFolderAsync() is not { } folder) return;
        if (!File.Exists(Path.Combine(folder.Path, "laya.int8.onnx")) && !File.Exists(Path.Combine(folder.Path, "laya.onnx")))
        {
            await Dialogs.ShowMessageAsync("不是 Laya 模型資料夾", "資料夾裡需要有 laya.int8.onnx 與 tokenizer.bin。");
            return;
        }
        layaDir_ = folder.Path;
        LayaCard.Description = layaDir_;
        ApplyButton.IsEnabled = true;
    }

    private async void Apply_Click(object sender, RoutedEventArgs e)
    {
        var ranker = (RankerBox.SelectedItem as ComboBoxItem)?.Tag as string ?? "auto";
        Native.WriteEngineSetting("model", "ranker", ranker);
        if (ModelBox.SelectedIndex >= 0) Native.WriteEngineSetting("model", "path", modelPaths_[ModelBox.SelectedIndex]);
        Native.WriteEngineSetting("model", "device", deviceNames_[Math.Max(0, DeviceBox.SelectedIndex)]);
        Native.WriteEngineSetting("model", "threads", ((int)ThreadsBox.Value).ToString());
        if (layaDir_.Length > 0) Native.WriteEngineSetting("laya", "path", layaDir_);
        ApplyButton.IsEnabled = false;
        var reloaded = await Task.Run(Native.ReloadEngine);
        if (!reloaded)
        {
            // Not running (or an older engine): the settings apply when it next starts.
            await Task.Run(Native.StartEngine);
        }
        BenchmarkText.Text = "";
        await RefreshStatusAsync();
    }

    private async void StartEngine_Click(object sender, RoutedEventArgs e)
    {
        StartEngineButton.IsEnabled = false;
        if (!await Task.Run(Native.StartEngine))
        {
            await Dialogs.ShowMessageAsync("無法啟動選字引擎",
                $"找不到 {Path.Combine(Native.InstallRoot, "engine", "LLaIME.Engine.exe")}。");
        }
        StartEngineButton.IsEnabled = true;
        await Task.Delay(500);
        await RefreshStatusAsync();
        if (DeviceBox.Items.Count <= 1) await LoadDevicesAsync(Native.ReadEngineSetting("model", "device"));
    }

    private async void Benchmark_Click(object sender, RoutedEventArgs e)
    {
        BenchmarkButton.IsEnabled = false;
        BenchmarkRing.IsActive = true;
        BenchmarkText.Text = "測速中…";
        var result = await Task.Run(() => Native.Benchmark(31));
        BenchmarkRing.IsActive = false;
        BenchmarkButton.IsEnabled = true;
        BenchmarkText.Text = result.Ok
            ? $"中位數 {result.P50:0} ms，95% 在 {result.P95:0} ms 以內"
            : result.Error ?? "測速失敗";
    }
}
