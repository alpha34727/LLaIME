using CommunityToolkit.WinUI.Controls;
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
    private readonly List<string> layaDeviceNames_ = [];
    // The engine lists the devices once it is running; until then only 自動 is offered, and the
    // status timer asks again.
    private bool devicesLoaded_;
    private bool devicesLoading_;
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
        ShowDownloads();
        layaDir_ = Native.ReadEngineSetting("laya", "path");
        LayaExpander.Description = layaDir_.Length > 0 ? layaDir_ : "尚未設定";
        ContextBox.Value = Native.ContextChars;
        ThreadsBox.Value = int.TryParse(Native.ReadEngineSetting("model", "threads"), out var t) ? t : 0;
        DeviceBox.Items.Clear();
        DeviceBox.Items.Add("自動");
        deviceNames_.Clear();
        deviceNames_.Add("auto");
        DeviceBox.SelectedIndex = 0;
        // Until the engine lists the devices: the CPU, and the one in the settings.
        var layaDevice = Native.ReadEngineSetting("laya", "device") is { Length: > 0 } ld ? ld : "CPU";
        LayaDeviceBox.Items.Clear();
        layaDeviceNames_.Clear();
        AddLayaDevice("CPU", "CPU");
        if (layaDevice != "CPU") AddLayaDevice(layaDevice, layaDevice);
        LayaDeviceBox.SelectedIndex = layaDeviceNames_.Count - 1;
        UpdateThreadsCard();
        devicesLoaded_ = false;
        loading_ = false;

        statusTimer_ = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        statusTimer_.Tick += async (_, _) => await RefreshStatusAsync();
        statusTimer_.Start();
        await RefreshStatusAsync();
        await LoadDevicesAsync(Native.ReadEngineSetting("model", "device"));
    }

    protected override void OnNavigatedFrom(NavigationEventArgs e)
    {
        statusTimer_?.Stop();
        DownloadChanged -= OnDownloadChanged;
    }

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
    // --- downloads --------------------------------------------------------------------------

    // One download at a time, kept across visits to the page.
    private static CatalogModel? downloading_;
    private static CancellationTokenSource? downloadCancel_;
    private static long downloadDone_;
    private static string? downloadError_;
    private static event Action? DownloadChanged;
    // Progress only touches the busy card in place; rebuilding the list on every update made
    // the page jump around.
    private static event Action? DownloadProgressed;
    private SettingsCard? busyCard_;
    private ProgressBar? busyBar_;
    private string busyHead_ = "";
    private CatalogModel? busyModel_;

    private void ShowDownloads()
    {
        DownloadChanged -= OnDownloadChanged;
        DownloadChanged += OnDownloadChanged;
        DownloadProgressed -= OnDownloadProgressed;
        DownloadProgressed += OnDownloadProgressed;
        busyCard_ = null;
        busyBar_ = null;
        DownloadList.Children.Clear();
        var catalog = ModelCatalog.Load();
        if (catalog.Count == 0)
        {
            DownloadList.Children.Add(new TextBlock { Text = "找不到模型清單（models\\catalog.json）。" });
            return;
        }
        foreach (var model in catalog) DownloadList.Children.Add(DownloadCard(model));
    }

    private void OnDownloadChanged() => DispatcherQueue.TryEnqueue(ShowDownloads);

    private void OnDownloadProgressed() => DispatcherQueue.TryEnqueue(() =>
    {
        if (busyCard_ == null || busyBar_ == null || busyModel_ == null) return;
        busyBar_.Value = downloadDone_;
        busyCard_.Description = busyHead_ + DownloadingStatus(busyModel_);
    });

    private static string DownloadingStatus(CatalogModel model) =>
        $"下載中 {downloadDone_ * 100.0 / model.Size:0}%（{Gb(downloadDone_)} / {Gb(model.Size)}）";

    private static string Gb(long bytes) => $"{bytes / 1e9:0.0} GB";

    private SettingsCard DownloadCard(CatalogModel model)
    {
        var current = model.Kind == "laya" ? Native.ReadEngineSetting("laya", "path") : Native.ReadEngineSetting("model", "path");
        var inUse = string.Equals(Path.GetFullPath(current.Length > 0 ? current : "."), Path.GetFullPath(model.EnginePath),
                                  StringComparison.OrdinalIgnoreCase);
        var busy = downloading_?.Id == model.Id;
        var status = !model.Published ? "尚未發布"
            : busy ? DownloadingStatus(model)
            : model.Downloaded ? (inUse ? "已下載 · 使用中" : "已下載")
            : downloadError_ != null && downloading_ == null && lastFailed_ == model.Id ? $"下載失敗：{downloadError_}"
            : $"{Gb(model.Size)}";
        var license = model.License.Length > 0 ? $" · 授權 {model.License}" : "";
        var card = new SettingsCard
        {
            Header = model.Name,
            Description = $"{model.Description}\n{status}{license}",
            HeaderIcon = new FontIcon { Glyph = model.Kind == "laya" ? "\uE9D9" : "\uE8F1" },
        };
        var panel = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        if (busy)
        {
            var bar = new ProgressBar
            {
                Width = 160, Minimum = 0, Maximum = model.Size, Value = downloadDone_,
                VerticalAlignment = VerticalAlignment.Center,
            };
            panel.Children.Add(bar);
            busyCard_ = card;
            busyBar_ = bar;
            busyModel_ = model;
            busyHead_ = $"{model.Description}\n";
            var cancel = new Button { Content = "取消" };
            cancel.Click += (_, _) => downloadCancel_?.Cancel();
            panel.Children.Add(cancel);
        }
        else if (model.Downloaded)
        {
            if (!inUse)
            {
                var delete = new Button { Content = new FontIcon { Glyph = "\uE74D", FontSize = 14 } };
                ToolTipService.SetToolTip(delete, "刪除下載的檔案");
                delete.Click += async (_, _) =>
                {
                    if (!await Dialogs.ConfirmAsync($"刪除「{model.Name}」？", model.Custom ? $"會刪除 {model.Folder}，並從清單移除。" : $"會刪除 {model.Folder}，之後可再下載。", "刪除")) return;
                    ModelCatalog.Delete(model);
                    ShowDownloads();
                };
                panel.Children.Add(delete);
                var use = new Button { Content = "使用" };
                use.Click += async (_, _) => await UseModelAsync(model);
                panel.Children.Add(use);
            }
        }
        else
        {
            var download = new Button
            {
                Content = model.Published ? "下載" : "尚未發布",
                IsEnabled = model.Published && downloading_ == null,
            };
            download.Click += async (_, _) => await DownloadAsync(model);
            if (model.Custom)
            {
                // A link that was tried but not downloaded (or whose download was left partway):
                // takes it, and what was downloaded of it, off the list.
                var remove = new Button
                {
                    Content = new FontIcon { Glyph = "\uE74D", FontSize = 14 },
                    IsEnabled = downloading_ == null,
                };
                ToolTipService.SetToolTip(remove, "從清單移除");
                remove.Click += (_, _) =>
                {
                    ModelCatalog.Delete(model);
                    ShowDownloads();
                };
                panel.Children.Add(remove);
            }
            panel.Children.Add(download);
        }
        card.Content = panel;
        return card;
    }

    private static string? lastFailed_;

    private async void LinkBox_KeyDown(object sender, Microsoft.UI.Xaml.Input.KeyRoutedEventArgs e)
    {
        if (e.Key == Windows.System.VirtualKey.Enter) await DownloadLinkAsync();
    }

    private async void LinkDownload_Click(object sender, RoutedEventArgs e) => await DownloadLinkAsync();

    // Looks the link up on the Hub, lets people pick a file if there are several (the Q4_K_M
    // quantization first), then downloads it like a catalog model.
    private async Task DownloadLinkAsync()
    {
        var link = LinkBox.Text.Trim();
        if (link.Length == 0) return;
        if (downloading_ != null)
        {
            await Dialogs.ShowMessageAsync("正在下載其他模型", "請等目前的下載完成或取消後再試。");
            return;
        }
        LinkButton.IsEnabled = false;
        LinkRing.IsActive = true;
        List<CatalogModel> choices;
        try
        {
            choices = await ModelCatalog.ResolveLinkAsync(link, CancellationToken.None);
        }
        catch (Exception ex) when (ex is InvalidDataException or HttpRequestException or System.Text.Json.JsonException)
        {
            await Dialogs.ShowMessageAsync("無法下載這個連結", ex is HttpRequestException ? $"無法連線到 Hugging Face：{ex.Message}" : ex.Message);
            return;
        }
        finally
        {
            LinkButton.IsEnabled = true;
            LinkRing.IsActive = false;
        }

        var model = choices[0];
        if (choices.Count > 1)
        {
            var list = new ListView
            {
                ItemsSource = choices.Select(c => $"{c.Name}（{Gb(c.Size)}）").ToList(),
                SelectionMode = ListViewSelectionMode.Single,
                MaxHeight = 360,
            };
            var preferred = choices.FindIndex(c => c.Name.Contains("Q4_K_M", StringComparison.OrdinalIgnoreCase));
            list.SelectedIndex = Math.Max(0, preferred);
            list.Loaded += (_, _) => list.ScrollIntoView(list.SelectedItem, ScrollIntoViewAlignment.Leading);
            var content = new StackPanel { Spacing = 8, MinWidth = 420 };
            content.Children.Add(new TextBlock
            {
                Text = "這個模型有多個檔案（不同的量化等級）。Q4_K_M 通常是大小與準確度最平衡的選擇。",
                TextWrapping = TextWrapping.Wrap,
            });
            content.Children.Add(list);
            if (await Dialogs.Create($"選擇要下載的檔案：{choices[0].Repo}", content, "下載", "取消").ShowAsync()
                != ContentDialogResult.Primary || list.SelectedIndex < 0)
            {
                return;
            }
            model = choices[list.SelectedIndex];
        }
        LinkBox.Text = "";
        await DownloadAsync(model);
    }

    private async Task DownloadAsync(CatalogModel model)
    {
        // A model from a link is listed (and resumable) from the moment its download starts.
        if (model.Custom) ModelCatalog.SaveManifest(model);
        downloading_ = model;
        downloadDone_ = 0;
        downloadError_ = null;
        downloadCancel_ = new CancellationTokenSource();
        DownloadChanged?.Invoke();
        var progress = new Progress<long>(done =>
        {
            downloadDone_ = done;
            DownloadProgressed?.Invoke();
        });
        var ok = false;
        try
        {
            await ModelCatalog.DownloadAsync(model, progress, downloadCancel_.Token);
            ok = true;
        }
        catch (OperationCanceledException)
        {
            // The partial file stays for the next attempt.
        }
        catch (Exception e)
        {
            downloadError_ = e.Message;
            lastFailed_ = model.Id;
        }
        downloading_ = null;
        downloadCancel_ = null;
        DownloadChanged?.Invoke();
        if (!ok) return;
        // The first model of its kind is used right away.
        var key = model.Kind == "laya" ? ("laya", "path") : ("model", "path");
        var current = Native.ReadEngineSetting(key.Item1, key.Item2);
        if (current.Length == 0 || !(File.Exists(current) || Directory.Exists(current))) await UseModelAsync(model);
    }

    private async Task UseModelAsync(CatalogModel model)
    {
        if (model.Kind == "laya")
        {
            Native.WriteEngineSetting("laya", "path", model.EnginePath);
            layaDir_ = model.EnginePath;
            LayaExpander.Description = layaDir_;
        }
        else
        {
            Native.WriteEngineSetting("model", "path", model.EnginePath);
            loading_ = true;
            LoadModels(model.EnginePath);
            loading_ = false;
        }
        if (!await Task.Run(Native.ReloadEngine)) await Task.Run(Native.StartEngine);
        ShowDownloads();
        await RefreshStatusAsync();
    }

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
        if (devicesLoaded_ || devicesLoading_) return;
        devicesLoading_ = true;
        try { await LoadDevicesCoreAsync(current); } finally { devicesLoading_ = false; }
    }

    private async Task LoadDevicesCoreAsync(string current)
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

        // Laya runs on ONNX Runtime: the CPU, or an NVIDIA GPU through its CUDA provider.
        var layaCurrent = layaDeviceNames_[Math.Max(0, LayaDeviceBox.SelectedIndex)];
        LayaDeviceBox.Items.Clear();
        layaDeviceNames_.Clear();
        var cpu = devices.FirstOrDefault(d => d.Name == "CPU");
        AddLayaDevice("CPU", cpu != null ? $"CPU（{cpu.Description}）" : "CPU");
        foreach (var d in devices.Where(d => d.Name.StartsWith("CUDA")))
        {
            var memory = d.Memory > 0 ? $"，{d.Memory / (1024.0 * 1024 * 1024):0.#} GB" : "";
            AddLayaDevice(d.Name, $"NVIDIA CUDA：{d.Description}{memory}");
        }
        var layaIndex = layaDeviceNames_.FindIndex(n => string.Equals(n, layaCurrent, StringComparison.OrdinalIgnoreCase));
        if (layaIndex < 0)
        {
            AddLayaDevice(layaCurrent, $"{layaCurrent}（目前不可用）");
            layaIndex = layaDeviceNames_.Count - 1;
        }
        LayaDeviceBox.SelectedIndex = layaIndex;
        UpdateThreadsCard();
        devicesLoaded_ = true;
        loading_ = false;
    }

    private async Task RefreshStatusAsync()
    {
        var status = await Task.Run(Native.EngineStatus);
        if (status.Running && !devicesLoaded_) _ = LoadDevicesAsync(Native.ReadEngineSetting("model", "device"));
        StartEngineButton.Visibility = status.Running ? Visibility.Collapsed : Visibility.Visible;
        BenchmarkButton.IsEnabled = status.Running && status.State == "ready" && status.Ranker != "none";
        if (!status.Running)
        {
            StatusBar.Severity = InfoBarSeverity.Warning;
            StatusBar.Title = "選字引擎沒有在執行";
            StatusBar.Message = "輸入法會在下次打字時自動啟動它，也可以現在啟動。";
        }
        else if (status.State == "ready" && status.Ranker == "none")
        {
            StatusBar.Severity = InfoBarSeverity.Informational;
            StatusBar.Title = "使用傳統選字演算法";
            StatusBar.Message = "不使用模型，依詞庫與學習的結果選字。";
        }
        else if (status.State == "ready")
        {
            StatusBar.Severity = InfoBarSeverity.Success;
            StatusBar.Title = status.Ranker == "laya" ? "已載入決策模型" : "已載入大語言模型";
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
            var noModel = Native.ReadEngineSetting("model", "path").Length == 0 &&
                          Native.ReadEngineSetting("laya", "path").Length == 0;
            StatusBar.Title = noModel ? "還沒有選字模型，目前只用詞庫選字" : StatusBar.Title;
            StatusBar.Message = noModel ? "請在下方「下載模型」選一個下載。"
                : status.Error.Length > 0 ? status.Error : "請檢查模型檔案與加速裝置設定。";
        }
    }

    private void AddLayaDevice(string name, string label)
    {
        layaDeviceNames_.Add(name);
        LayaDeviceBox.Items.Add(label);
    }

    // Threads only matter for what runs on the CPU: shown for 自動 and CPU.
    private void UpdateThreadsCard()
    {
        var device = DeviceBox.SelectedIndex >= 0 && DeviceBox.SelectedIndex < deviceNames_.Count
            ? deviceNames_[DeviceBox.SelectedIndex] : "auto";
        ThreadsCard.Visibility = device is "auto" or "CPU" ? Visibility.Visible : Visibility.Collapsed;
    }

    private void Setting_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (ReferenceEquals(sender, DeviceBox)) UpdateThreadsCard();
        if (ReferenceEquals(sender, ModelBox)) UpdateModelPath();
        if (!loading_) ApplyButton.IsEnabled = true;
    }

    // Takes effect at once: the IME reads it from the registry at the next composition.
    private void Context_Changed(NumberBox sender, NumberBoxValueChangedEventArgs args)
    {
        if (double.IsNaN(sender.Value)) sender.Value = 32;
        if (!loading_) Native.ContextChars = (int)sender.Value;
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
        LayaExpander.Description = layaDir_;
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
        if (LayaDeviceBox.SelectedIndex >= 0) Native.WriteEngineSetting("laya", "device", layaDeviceNames_[LayaDeviceBox.SelectedIndex]);
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
