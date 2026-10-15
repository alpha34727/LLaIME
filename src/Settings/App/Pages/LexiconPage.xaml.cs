using CommunityToolkit.WinUI.Controls;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;

namespace LLaIME.Settings.Pages;

// The lexicon layers (src/Core/include/llaime/LexiconCatalog.h): built-in ones can be turned
// off (except the main table), imported ones also deleted.
public sealed partial class LexiconPage : Page
{
    public LexiconPage()
    {
        InitializeComponent();
    }

    protected override void OnNavigatedTo(NavigationEventArgs e) => Refresh();

    private void Refresh()
    {
        LexiconList.Children.Clear();
        foreach (var info in Native.ListLexicons()) LexiconList.Children.Add(Card(info));
    }

    private SettingsCard Card(LexiconInfo info)
    {
        var details = new List<string> { $"{info.Entries:N0} 個詞" };
        if (info.License.Length > 0) details.Add($"授權：{info.License}");
        details.Add(info.Required ? "固定啟用" : info.BuiltIn ? "內建" : "匯入");
        var description = info.Description.Length > 0 ? info.Description + "\n" : "";
        var card = new SettingsCard
        {
            Header = info.Name,
            Description = description + string.Join(" · ", details),
            HeaderIcon = new FontIcon { Glyph = info.BuiltIn ? "" : "" },
        };
        if (info.Source.StartsWith("http")) ToolTipService.SetToolTip(card, info.Source);

        var toggle = new ToggleSwitch
        {
            IsOn = info.Enabled,
            IsEnabled = !info.Required,
            OnContent = "使用中",
            OffContent = "不使用",
        };
        toggle.Toggled += (_, _) =>
        {
            if (!Native.SetLexiconEnabled(info.Id, toggle.IsOn)) toggle.IsOn = !toggle.IsOn;
        };
        var panel = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        if (!info.BuiltIn)
        {
            var delete = new Button { Content = new FontIcon { Glyph = "", FontSize = 14 } };
            ToolTipService.SetToolTip(delete, "刪除這個詞庫");
            delete.Click += async (_, _) =>
            {
                if (!await Dialogs.ConfirmAsync($"刪除「{info.Name}」？", "這個匯入的詞庫會從電腦上刪除。原本的詞表檔案不受影響。", "刪除")) return;
                Native.DeleteLexicon(info.Id);
                Refresh();
            };
            panel.Children.Add(delete);
        }
        panel.Children.Add(toggle);
        card.Content = panel;
        return card;
    }

    // Words and their readings in two aligned columns.
    private static Grid SampleTable(List<ImportSample> sample)
    {
        var grid = new Grid { ColumnSpacing = 24, RowSpacing = 4 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        var secondary = (Microsoft.UI.Xaml.Media.Brush)Application.Current.Resources["TextFillColorSecondaryBrush"];
        for (var i = 0; i < sample.Count; ++i)
        {
            grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
            var text = new TextBlock { Text = sample[i].Text };
            var reading = new TextBlock { Text = sample[i].Reading, Foreground = secondary };
            Grid.SetRow(text, i);
            Grid.SetRow(reading, i);
            Grid.SetColumn(reading, 1);
            grid.Children.Add(text);
            grid.Children.Add(reading);
        }
        return grid;
    }

    private async void Import_Click(object sender, RoutedEventArgs e)
    {
        var path = await Dialogs.PickFileAsync(".txt", ".csv", ".src", ".tsv", "*");
        if (path == null) return;
        var preview = await Task.Run(() => Native.PreviewImport(path));
        if (!preview.Ok)
        {
            await Dialogs.ShowMessageAsync("無法匯入", preview.Error ?? "");
            return;
        }
        if (preview.Phrases == 0)
        {
            await Dialogs.ShowMessageAsync("沒有可以匯入的詞", "檔案中找不到詞，或所有的詞都無法標注音。");
            return;
        }

        var name = new TextBox { Header = "名稱", Text = Path.GetFileNameWithoutExtension(path) };
        var license = new TextBox { Header = "授權（選填）", PlaceholderText = "例如 CC BY 4.0" };
        var description = new TextBox { Header = "說明（選填）" };
        var summary = $"共 {preview.Phrases:N0} 個詞";
        if (preview.Guessed > 0) summary += $"，其中 {preview.Guessed:N0} 個依詞庫自動標注音";
        if (preview.UnreadableCount > 0) summary += $"；{preview.UnreadableCount:N0} 個無法標注音，將略過";
        var content = new StackPanel { Spacing = 12, MinWidth = 420 };
        content.Children.Add(new TextBlock { Text = summary + "。", TextWrapping = TextWrapping.Wrap });
        content.Children.Add(name);
        content.Children.Add(license);
        content.Children.Add(description);
        content.Children.Add(new TextBlock { Text = "預覽", Style = (Style)Application.Current.Resources["BodyStrongTextBlockStyle"] });
        content.Children.Add(new ScrollViewer { Content = SampleTable(preview.Sample), MaxHeight = 180 });
        if (preview.UnreadableCount > 0)
        {
            content.Children.Add(new Expander
            {
                Header = "無法標注音的詞",
                HorizontalAlignment = HorizontalAlignment.Stretch,
                Content = new TextBlock { Text = string.Join("、", preview.Unreadable), TextWrapping = TextWrapping.Wrap },
            });
        }
        var dialog = Dialogs.Create("匯入詞庫", new ScrollViewer { Content = content }, "匯入", "取消");
        if (await dialog.ShowAsync() != ContentDialogResult.Primary) return;

        // Controls are read on the UI thread; the import itself runs in the background.
        var (n, d, l) = (name.Text.Trim(), description.Text.Trim(), license.Text.Trim());
        var result = await Task.Run(() => Native.ImportLexicon(path, n, d, l));
        if (!result.Ok)
        {
            await Dialogs.ShowMessageAsync("無法匯入", result.Error ?? "");
            return;
        }
        Refresh();
    }
}
