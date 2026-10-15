using System.Runtime.InteropServices;
using LLaIME.Settings.Pages;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Animation;
using Windows.Graphics;

namespace LLaIME.Settings;

public sealed partial class MainWindow : Window
{
    private static readonly Dictionary<string, Type> Pages = new()
    {
        ["general"] = typeof(GeneralPage),
        ["model"] = typeof(ModelPage),
        ["lexicons"] = typeof(LexiconPage),
        ["userdict"] = typeof(UserDictPage),
        ["about"] = typeof(AboutPage),
    };

    public MainWindow()
    {
        InitializeComponent();
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);
        AppWindow.TitleBar.PreferredHeightOption = TitleBarHeightOption.Tall;
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "AppIcon.ico"));

        var scale = GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(this)) / 96.0;
        AppWindow.Resize(new SizeInt32((int)(1080 * scale), (int)(760 * scale)));
        if (AppWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.PreferredMinimumWidth = (int)(640 * scale);
            presenter.PreferredMinimumHeight = (int)(480 * scale);
        }
        ContentFrame.Navigate(typeof(GeneralPage));
    }

    public XamlRoot? Xaml => Content.XamlRoot;
    public nint Handle => WinRT.Interop.WindowNative.GetWindowHandle(this);

    public void BringToFront()
    {
        if (AppWindow.Presenter is OverlappedPresenter { State: OverlappedPresenterState.Minimized } p) p.Restore();
        Activate();
        SetForegroundWindow(Handle);
    }

    // Selects a page by its tag ("general", "model", "lexicons", "userdict", "about").
    public void Navigate(string tag)
    {
        foreach (var item in Nav.MenuItems.Concat(Nav.FooterMenuItems))
        {
            if (item is NavigationViewItem { Tag: string t } nav && t == tag) Nav.SelectedItem = nav;
        }
    }

    private void Nav_SelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (args.SelectedItem is NavigationViewItem { Tag: string tag } && Pages.TryGetValue(tag, out var page) &&
            ContentFrame.CurrentSourcePageType != page)
        {
            ContentFrame.Navigate(page, null, new EntranceNavigationTransitionInfo());
        }
    }

    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(nint hwnd);
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(nint hwnd);
}
