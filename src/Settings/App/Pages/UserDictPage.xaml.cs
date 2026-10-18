using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Navigation;

namespace LLaIME.Settings.Pages;

// The user's phrases and learned picks in user.db. Edits go straight to the database (WAL);
// the engine notices the change and every app's IME picks it up at its next composition.
public sealed partial class UserDictPage : Page
{
    private List<PhraseInfo> phrases_ = [];
    private List<SelectionInfo> selections_ = [];
    private string phraseSort_ = "created";
    private bool phraseSortDescending_ = true;
    private bool loading_;

    public UserDictPage()
    {
        InitializeComponent();
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        loading_ = true;
        LearningSwitch.IsOn = Native.LearningEnabled;
        loading_ = false;
        if (e.Parameter as string == "learning") Tabs.SelectedItem = LearningTab;
        RefreshPhrases();
        RefreshSelections();
    }

    private void Tabs_SelectionChanged(SelectorBar sender, SelectorBarSelectionChangedEventArgs args)
    {
        var learning = sender.SelectedItem == LearningTab;
        PhrasesPanel.Visibility = learning ? Visibility.Collapsed : Visibility.Visible;
        LearningPanel.Visibility = learning ? Visibility.Visible : Visibility.Collapsed;
        if (learning) RefreshSelections(); else RefreshPhrases();
    }

    // --- phrases ------------------------------------------------------------------------------

    private void RefreshPhrases()
    {
        phrases_ = Native.ListPhrases();
        ShowPhrases();
    }

    private static bool Matches(string query, string text, string reading) =>
        query.Length == 0 || text.Contains(query, StringComparison.OrdinalIgnoreCase) ||
        reading.Replace(" ", "").Contains(query.Replace(" ", ""), StringComparison.OrdinalIgnoreCase);

    private void ShowPhrases()
    {
        var query = PhraseSearch.Text.Trim();
        IEnumerable<PhraseInfo> list = phrases_.Where(p => Matches(query, p.Text, p.Reading) ||
                                                           (query.Length > 0 && p.Shortcut.StartsWith(query, StringComparison.OrdinalIgnoreCase)));
        Func<PhraseInfo, object> key = phraseSort_ switch
        {
            "text" => p => p.Text,
            "reading" => p => p.Reading,
            "kind" => p => p.Kind,
            "shortcut" => p => p.Shortcut,
            _ => p => p.Created,
        };
        list = phraseSortDescending_ ? list.OrderByDescending(key) : list.OrderBy(key);
        PhraseList.ItemsSource = list.ToList();
        PhrasesEmpty.Visibility = phrases_.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        UpdatePhraseButtons();
    }

    private IList<PhraseInfo> SelectedPhrases => PhraseList.SelectedItems.Cast<PhraseInfo>().ToList();

    private void UpdatePhraseButtons()
    {
        var selected = SelectedPhrases;
        EditPhraseButton.IsEnabled = selected.Count == 1;
        DeletePhraseButton.IsEnabled = selected.Count > 0;
        MakeManualButton.IsEnabled = selected.Any(p => p.IsLearned);
    }

    private void PhraseSearch_TextChanged(AutoSuggestBox sender, AutoSuggestBoxTextChangedEventArgs args) => ShowPhrases();
    private void PhraseList_SelectionChanged(object sender, SelectionChangedEventArgs e) => UpdatePhraseButtons();

    private void PhraseSort_Click(object sender, RoutedEventArgs e)
    {
        var tag = (string)((Button)sender).Tag;
        phraseSortDescending_ = tag == phraseSort_ ? !phraseSortDescending_ : tag == "created";
        phraseSort_ = tag;
        ShowPhrases();
    }

    private async void AddPhrase_Click(object sender, RoutedEventArgs e) => await EditPhraseAsync(null);

    private async void EditPhrase_Click(object sender, RoutedEventArgs e)
    {
        if (SelectedPhrases is [var phrase]) await EditPhraseAsync(phrase);
    }

    private async void PhraseList_DoubleTapped(object sender, DoubleTappedRoutedEventArgs e)
    {
        if ((e.OriginalSource as FrameworkElement)?.DataContext is PhraseInfo phrase) await EditPhraseAsync(phrase);
    }

    // Adds a phrase (existing == null) or edits one. The reading is filled in from the lexicon
    // as the text is typed; heteronyms offer their other readings in the list. A shortcut types
    // the phrase with ` p and its keys; a phrase with a shortcut needs no reading (「」).
    private async Task EditPhraseAsync(PhraseInfo? existing)
    {
        var text = new TextBox { Header = "詞", Text = existing?.Text ?? "", PlaceholderText = "例如：魏哲家", MaxLength = 32 };
        var reading = new ComboBox
        {
            Header = "注音（以空白分隔音節，一聲不標）",
            IsEditable = true,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            PlaceholderText = "輸入詞後自動標注音",
        };
        var shortcut = new TextBox
        {
            Header = "快捷鍵（選填）",
            Text = existing?.Shortcut ?? "",
            PlaceholderText = "例如 mk：輸入時依序按 `、p、m、k",
            MaxLength = 16,
            FontFamily = new Microsoft.UI.Xaml.Media.FontFamily("Consolas"),
        };
        var hint = new TextBlock
        {
            TextWrapping = TextWrapping.Wrap,
            Foreground = (Microsoft.UI.Xaml.Media.Brush)Application.Current.Resources["TextFillColorSecondaryBrush"],
        };
        var content = new StackPanel { Spacing = 12, MinWidth = 380 };
        content.Children.Add(text);
        content.Children.Add(reading);
        content.Children.Add(shortcut);
        content.Children.Add(hint);
        var dialog = Dialogs.Create(existing == null ? "新增詞彙" : "編輯詞彙", content, "儲存", "取消");

        string ReadingText() => (reading.Text ?? "").Trim();
        string ShortcutKeys() => shortcut.Text.Trim().ToLowerInvariant();
        void Validate()
        {
            var keys = ShortcutKeys();
            var keysOk = keys.Length == 0 || keys.All(c => c is >= 'a' and <= 'z' or >= '0' and <= '9');
            var readingOk = ReadingText().Length == 0
                ? keys.Length > 0
                : Native.CheckReading(ReadingText(), text.Text.Trim());
            var ok = text.Text.Trim().Length > 0 && readingOk && keysOk;
            dialog.IsPrimaryButtonEnabled = ok;
            hint.Text = text.Text.Trim().Length == 0 ? ""
                : !keysOk ? "快捷鍵只能用英文字母與數字。"
                : ReadingText().Length == 0 && keys.Length == 0 ? "詞庫裡找不到這些字的讀音，請自行輸入注音，或只指定快捷鍵（例如符號「」）。"
                : ReadingText().Length == 0 ? $"沒有注音：只能用快捷鍵 ` p {keys} 輸入。"
                : !readingOk ? "注音的音節數要和字數相同，例如「ㄨㄟˋ ㄓㄜˊ ㄐㄧㄚ」。"
                : keys.Length > 0 ? $"也可以依序按 `、p、{string.Join("、", keys.ToCharArray())} 輸入。"
                : reading.Items.Count > 1 ? "有多種讀音，可從清單中選擇。" : "";
        }
        void Suggest()
        {
            var guesses = Native.GuessReadings(text.Text.Trim());
            reading.Items.Clear();
            foreach (var g in guesses) reading.Items.Add(g);
            reading.Text = guesses.FirstOrDefault() ?? "";
            if (guesses.Count > 0) reading.SelectedIndex = 0;
            Validate();
        }
        text.TextChanged += (_, _) => Suggest();
        shortcut.TextChanged += (_, _) => Validate();
        reading.TextSubmitted += (_, args) => DispatcherQueue.TryEnqueue(Validate);
        reading.SelectionChanged += (_, _) => DispatcherQueue.TryEnqueue(Validate);
        reading.KeyUp += (_, _) => Validate();

        if (existing != null)
        {
            Suggest();
            shortcut.Text = existing.Shortcut;
            if (existing.Reading.Length > 0)
            {
                if (!reading.Items.Contains(existing.Reading)) reading.Items.Insert(0, existing.Reading);
                reading.SelectedItem = existing.Reading;
            }
            else
            {
                reading.SelectedIndex = -1;  // typed by its shortcut alone
            }
            reading.Text = existing.Reading;
        }
        Validate();
        if (await dialog.ShowAsync() != ContentDialogResult.Primary) return;

        var result = existing == null
            ? Native.AddPhrase(ReadingText(), text.Text.Trim(), ShortcutKeys())
            : Native.UpdatePhrase(existing, ReadingText(), text.Text.Trim(), ShortcutKeys());
        if (!result.Ok) await Dialogs.ShowMessageAsync("無法儲存", result.Error ?? "請檢查詞與注音。");
        RefreshPhrases();
    }

    private async void DeletePhrases_Click(object sender, RoutedEventArgs e)
    {
        var selected = SelectedPhrases;
        if (selected.Count == 0) return;
        var what = selected.Count == 1 ? $"「{selected[0].Text}」" : $"這 {selected.Count} 個詞";
        if (!await Dialogs.ConfirmAsync($"刪除{what}？", "刪除後輸入法不再把它當成一個詞。", "刪除")) return;
        foreach (var p in selected) Native.RemovePhrase(p);
        RefreshPhrases();
    }

    private void MakeManual_Click(object sender, RoutedEventArgs e)
    {
        foreach (var p in SelectedPhrases.Where(p => p.IsLearned)) Native.MakeManual(p);
        RefreshPhrases();
    }

    private async void ImportPhrases_Click(object sender, RoutedEventArgs e)
    {
        var path = await Dialogs.PickFileAsync(".txt", ".csv", ".src", ".tsv", "*");
        if (path == null) return;
        var result = await Task.Run(() => Native.ImportPhrases(path));
        if (!result.Ok)
        {
            await Dialogs.ShowMessageAsync("無法匯入", result.Error ?? "");
            return;
        }
        var message = $"檔案中有 {result.Phrases:N0} 個詞，新加入 {result.Added:N0} 個。";
        if (result.Guessed > 0) message += $"\n其中 {result.Guessed:N0} 個依詞庫自動標注音。";
        if (result.UnreadableCount > 0)
        {
            message += $"\n{result.UnreadableCount:N0} 個無法標注音，已略過：{string.Join("、", result.Unreadable)}";
        }
        await Dialogs.ShowMessageAsync("匯入完成", message);
        RefreshPhrases();
    }

    private async void ExportPhrases_Click(object sender, RoutedEventArgs e)
    {
        var includeLearned = new CheckBox { Content = "包含自動成詞", IsChecked = true };
        var content = new StackPanel { Spacing = 12 };
        content.Children.Add(new TextBlock
        {
            Text = "匯出成 UTF-8 文字檔，每行「詞 注音」（McBopomofo 使用者詞彙格式），可再匯入 LLaIME 或小麥注音。",
            TextWrapping = TextWrapping.Wrap,
        });
        content.Children.Add(includeLearned);
        if (await Dialogs.Create("匯出詞彙", content, "選擇位置…", "取消").ShowAsync() != ContentDialogResult.Primary) return;
        var path = await Dialogs.PickSaveFileAsync("LLaIME 使用者詞彙", "文字檔", ".txt");
        if (path == null) return;
        var count = Native.ExportPhrases(path, includeLearned.IsChecked == true);
        await Dialogs.ShowMessageAsync(count >= 0 ? "匯出完成" : "無法匯出",
            count >= 0 ? $"已匯出 {count:N0} 個詞到 {path}。" : $"無法寫入 {path}。");
    }

    // --- learned picks ------------------------------------------------------------------------

    private void RefreshSelections()
    {
        selections_ = Native.ListSelections();
        ShowSelections();
    }

    private void ShowSelections()
    {
        var query = SelectionSearch.Text.Trim();
        SelectionList.ItemsSource = selections_.Where(s => Matches(query, s.Text, s.Reading) || s.Prev.Contains(query)).ToList();
        SelectionsEmpty.Visibility = selections_.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
        DeleteSelectionButton.IsEnabled = false;
    }

    private void SelectionSearch_TextChanged(AutoSuggestBox sender, AutoSuggestBoxTextChangedEventArgs args) => ShowSelections();

    private void SelectionList_SelectionChanged(object sender, SelectionChangedEventArgs e) =>
        DeleteSelectionButton.IsEnabled = SelectionList.SelectedItems.Count > 0;

    private void DeleteSelections_Click(object sender, RoutedEventArgs e)
    {
        foreach (var s in SelectionList.SelectedItems.Cast<SelectionInfo>().ToList()) Native.RemoveSelection(s);
        RefreshSelections();
    }

    private async void ClearLearning_Click(object sender, RoutedEventArgs e)
    {
        if (!await Dialogs.ConfirmAsync("清除全部學習紀錄？",
                "所有改選紀錄與自動成詞都會刪除，而且無法復原。您手動加入的詞彙會保留。", "全部清除")) return;
        if (!await Task.Run(Native.ClearLearning)) await Dialogs.ShowMessageAsync("無法清除", "使用者詞庫正在使用中，請稍後再試。");
        RefreshSelections();
        RefreshPhrases();
    }

    private void LearningSwitch_Toggled(object sender, RoutedEventArgs e)
    {
        if (!loading_) Native.LearningEnabled = LearningSwitch.IsOn;
    }
}
