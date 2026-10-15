using Microsoft.UI.Xaml.Controls;

namespace LLaIME.Settings.Pages;

public sealed partial class AboutPage : Page
{
    public AboutPage()
    {
        InitializeComponent();
        VersionText.Text = $"版本 {typeof(AboutPage).Assembly.GetName().Version?.ToString(3)}";
        InstallCard.Description = Native.InstallRoot;
        DataCard.Description = Native.Paths.DataDir;
    }
}
