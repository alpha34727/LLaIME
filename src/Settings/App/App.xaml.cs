using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.Windows.AppLifecycle;

namespace LLaIME.Settings;

public partial class App : Application
{
    public static MainWindow? Window { get; private set; }

    public App()
    {
        InitializeComponent();
        // An error in one action should not close the window; it is logged and shown instead.
        UnhandledException += async (_, e) =>
        {
            e.Handled = true;
            try
            {
                File.AppendAllText(Path.Combine(Path.GetTempPath(), "LLaIME.Settings.log"),
                    $"{DateTime.Now:s} {e.Exception}\n");
                await Dialogs.ShowMessageAsync("發生錯誤", e.Message);
            }
            catch (Exception)
            {
                // A dialog may already be open.
            }
        };
    }

    // One settings window: a second launch (e.g. from the language bar menu) brings the first
    // one forward instead.
    [STAThread]
    private static void Main(string[] args)
    {
        WinRT.ComWrappersSupport.InitializeComWrappers();
        var instance = AppInstance.FindOrRegisterForKey("LLaIME.Settings");
        if (!instance.IsCurrent)
        {
            instance.RedirectActivationToAsync(AppInstance.GetCurrent().GetActivatedEventArgs())
                .AsTask().Wait();
            return;
        }
        instance.Activated += (_, _) => Window?.DispatcherQueue.TryEnqueue(() => Window?.BringToFront());
        Application.Start(_ =>
        {
            var context = new DispatcherQueueSynchronizationContext(DispatcherQueue.GetForCurrentThread());
            SynchronizationContext.SetSynchronizationContext(context);
            new App();
        });
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        Native.Initialize();
        Window = new MainWindow();
        Window.Activate();
    }
}
