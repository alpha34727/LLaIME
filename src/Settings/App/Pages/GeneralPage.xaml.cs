using System.Diagnostics;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Navigation;

namespace LLaIME.Settings.Pages;

public sealed partial class GeneralPage : Page
{
    private bool loading_;

    public GeneralPage()
    {
        InitializeComponent();
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        loading_ = true;
        LearningSwitch.IsOn = Native.LearningEnabled;
        DataFolderCard.Description = Native.Paths.DataDir + "（user.db、engine.ini、匯入的詞庫）";
        loading_ = false;
    }

    private void LearningSwitch_Toggled(object sender, RoutedEventArgs e)
    {
        if (!loading_) Native.LearningEnabled = LearningSwitch.IsOn;
    }

    private void UserDict_Click(object sender, RoutedEventArgs e) => App.Window?.Navigate("userdict");

    private void DataFolder_Click(object sender, RoutedEventArgs e)
    {
        Directory.CreateDirectory(Native.Paths.DataDir);
        Process.Start(new ProcessStartInfo("explorer.exe", $"\"{Native.Paths.DataDir}\"") { UseShellExecute = true });
    }
}
