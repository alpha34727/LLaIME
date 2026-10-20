using System.Text.RegularExpressions;
using CommunityToolkit.WinUI.Controls;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;

namespace LLaIME.Settings.Pages;

// Models from installer/models.json (installed as models\catalog.json) and from Hugging Face
// links, downloaded into %LOCALAPPDATA%\LLaIME\models. Each card shows only size, quantization
// and license, plus the download state; the longer description is its tooltip.
public sealed partial class DownloadPage : Page
{
    // One download at a time, kept across visits to the page.
    private static CatalogModel? downloading_;
    private static CancellationTokenSource? downloadCancel_;
    private static long downloadDone_;
    private static string? downloadError_;
    private static string? lastFailed_;
    private static event Action? DownloadChanged;
    // Progress only touches the busy card in place; rebuilding the list on every update made
    // the page jump around.
    private static event Action? DownloadProgressed;
    private SettingsCard? busyCard_;
    private ProgressBar? busyBar_;
    private CatalogModel? busyModel_;

    public DownloadPage()
    {
        InitializeComponent();
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        DownloadChanged += OnDownloadChanged;
        DownloadProgressed += OnDownloadProgressed;
        ShowDownloads();
    }

    protected override void OnNavigatedFrom(NavigationEventArgs e)
    {
        DownloadChanged -= OnDownloadChanged;
        DownloadProgressed -= OnDownloadProgressed;
    }

    private void ShowDownloads()
    {
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
        busyCard_.Description = Facts(busyModel_, DownloadingStatus(busyModel_));
    });

    private static string DownloadingStatus(CatalogModel model) =>
        $"下載中 {downloadDone_ * 100.0 / model.Size:0}%";

    private static string Gb(long bytes) => bytes >= 1e9 ? $"{bytes / 1e9:0.0} GB" : $"{bytes / 1e6:0} MB";

    private static readonly Regex QuantPattern =
        new(@"(?i)(IQ\d_[A-Z0-9_]+|Q\d_K(_[SML])?|Q\d_\d|BF16|F16|F32|FP16|INT8)");

    // The quantization: from the catalog, or read off a linked file's name.
    private static string Quant(CatalogModel model)
    {
        if (model.Quant.Length > 0) return model.Quant;
        var match = QuantPattern.Match(model.Files.Count > 0 ? model.Files[0].Name : "");
        return match.Success ? match.Value.ToUpperInvariant() : "";
    }

    // 1.3 GB · Q4_K_M · Apache-2.0, then the state when there is one.
    private static string Facts(CatalogModel model, string state)
    {
        var parts = new List<string> { Gb(model.Size) };
        if (Quant(model) is { Length: > 0 } quant) parts.Add(quant);
        if (model.License.Length > 0) parts.Add(model.License);
        var facts = string.Join(" · ", parts);
        return state.Length > 0 ? $"{facts}　{state}" : facts;
    }

    private SettingsCard DownloadCard(CatalogModel model)
    {
        var current = model.Kind == "laya" ? Native.ReadEngineSetting("laya", "path") : Native.ReadEngineSetting("model", "path");
        var inUse = string.Equals(Path.GetFullPath(current.Length > 0 ? current : "."), Path.GetFullPath(model.EnginePath),
                                  StringComparison.OrdinalIgnoreCase);
        var busy = downloading_?.Id == model.Id;
        var state = !model.Published ? "尚未發布"
            : busy ? DownloadingStatus(model)
            : model.Downloaded ? (inUse ? "使用中" : "已下載")
            : downloadError_ != null && downloading_ == null && lastFailed_ == model.Id ? $"下載失敗：{downloadError_}"
            : "";
        var card = new SettingsCard
        {
            Header = model.Name,
            Description = Facts(model, state),
            HeaderIcon = new FontIcon { Glyph = model.Kind == "laya" ? "" : "" },
        };
        if (model.Description.Length > 0) ToolTipService.SetToolTip(card, model.Description);
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
            var cancel = new Button { Content = "取消" };
            cancel.Click += (_, _) => downloadCancel_?.Cancel();
            panel.Children.Add(cancel);
        }
        else if (model.Downloaded)
        {
            if (!inUse)
            {
                var delete = new Button { Content = new FontIcon { Glyph = "", FontSize = 14 } };
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
                    Content = new FontIcon { Glyph = "", FontSize = 14 },
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
            // The GPU file goes with the GPU, as the settings page offers it.
            if (model.Files.Any(f => f.Name == "laya.fp16.onnx") && model.Files.All(f => f.Name != "laya.int8.onnx"))
            {
                var gpu = (await Task.Run(Native.EngineDevices))?.FirstOrDefault(d => d.Name.StartsWith("CUDA"));
                if (gpu != null) Native.WriteEngineSetting("laya", "device", gpu.Name);
            }
        }
        else
        {
            Native.WriteEngineSetting("model", "path", model.EnginePath);
        }
        if (!await Task.Run(Native.ReloadEngine)) await Task.Run(Native.StartEngine);
        ShowDownloads();
    }
}
