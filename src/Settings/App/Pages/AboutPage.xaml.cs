using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace LLaIME.Settings.Pages;

public sealed partial class AboutPage : Page
{
    private enum UpdateState { Idle, Checking, Downloading }

    private UpdateState state_ = UpdateState.Idle;
    private CancellationTokenSource? cancel_;

    public AboutPage()
    {
        InitializeComponent();
        VersionText.Text = $"版本 {Updater.Current.ToString(3)}";
        InstallCard.Description = Native.InstallRoot;
        DataCard.Description = Native.Paths.DataDir;
#if !INCLUDE_MOE
        // Builds without the Ministry of Education layer do not use its data.
        MoeCard.Visibility = Visibility.Collapsed;
#endif
        Unloaded += (_, _) => cancel_?.Cancel();
    }

    private async void Update_Click(object sender, RoutedEventArgs e)
    {
        if (state_ != UpdateState.Idle)
        {
            cancel_?.Cancel();
            return;
        }
        cancel_ = new CancellationTokenSource();
        var cancel = cancel_.Token;
        try
        {
            SetState(UpdateState.Checking, "正在檢查更新…");
            var release = await Updater.CheckAsync(cancel);
            if (!release.IsNewer)
            {
                SetState(UpdateState.Idle, $"已是最新版本（{Updater.Current.ToString(3)}）。");
                return;
            }
            SetState(UpdateState.Idle, $"有新版本 {release.Version.ToString(3)}。");
            if (release.Installer == null)
            {
                // This build's variant is not published (e.g. a build with the Ministry of
                // Education layer), or GitHub gave no checksum for it.
                if (await Dialogs.ConfirmAsync($"有新版本 {release.Version.ToString(3)}",
                        "發布頁面上沒有適合這個版本的安裝檔（或缺少檢查碼），請到發布頁面查看。", "開啟發布頁面"))
                {
                    Updater.OpenPage(release.PageUrl);
                }
                return;
            }
            if (!await ConfirmDownloadAsync(release)) return;

            SetState(UpdateState.Downloading, $"正在下載 {release.Version.ToString(3)}…");
            UpdateProgress.IsIndeterminate = false;
            UpdateProgress.Maximum = release.Installer.Size;
            var progress = new Progress<long>(done =>
            {
                UpdateProgress.Value = done;
                UpdateCard.Description = $"正在下載 {release.Version.ToString(3)}：{done / 1048576.0:N0} / {release.Installer.Size / 1048576.0:N0} MB";
            });
            var path = await Updater.DownloadAsync(release, progress, cancel);
            SetState(UpdateState.Idle, $"{release.Version.ToString(3)} 已下載。");
            if (!await Dialogs.ConfirmAsync("安裝更新",
                    "安裝程式會關閉設定與選字引擎，並需要系統管理員權限。安裝完成後，已經開著的應用程式要重新開啟，才會用到新版輸入法。",
                    "安裝"))
            {
                return;
            }
            Updater.StartInstaller(path);
            Application.Current.Exit();
        }
        catch (OperationCanceledException)
        {
            SetState(UpdateState.Idle, "已取消。下次會從中斷的地方繼續下載。");
        }
        catch (Exception ex) when (ex is InvalidDataException or HttpRequestException or IOException or UnauthorizedAccessException)
        {
            SetState(UpdateState.Idle, "無法更新：" + ex.Message);
        }
        catch (System.ComponentModel.Win32Exception ex)
        {
            SetState(UpdateState.Idle, "無法啟動安裝程式：" + ex.Message);
        }
    }

    private static async Task<bool> ConfirmDownloadAsync(Updater.Release release)
    {
        var panel = new StackPanel { Spacing = 8 };
        panel.Children.Add(new TextBlock
        {
            Text = $"目前版本 {Updater.Current.ToString(3)}，新版本 {release.Version.ToString(3)}。安裝檔約 {release.Installer!.Size / 1048576.0:N0} MB。",
            TextWrapping = TextWrapping.Wrap,
        });
        if (release.Notes.Length > 0)
        {
            panel.Children.Add(new ScrollViewer
            {
                MaxHeight = 240,
                Content = new TextBlock { Text = release.Notes, TextWrapping = TextWrapping.Wrap, IsTextSelectionEnabled = true },
            });
        }
        var dialog = Dialogs.Create($"有新版本 {release.Version.ToString(3)}", panel, "下載", "稍後");
        dialog.SecondaryButtonText = "發布頁面";
        var result = await dialog.ShowAsync();
        if (result == ContentDialogResult.Secondary) Updater.OpenPage(release.PageUrl);
        return result == ContentDialogResult.Primary;
    }

    private void SetState(UpdateState state, string description)
    {
        state_ = state;
        UpdateCard.Description = description;
        UpdateButton.Content = state == UpdateState.Idle ? "檢查更新" : "取消";
        UpdateProgress.Visibility = state == UpdateState.Idle ? Visibility.Collapsed : Visibility.Visible;
        UpdateProgress.IsIndeterminate = state == UpdateState.Checking;
    }
}
