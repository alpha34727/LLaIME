using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Storage.Pickers;

namespace LLaIME.Settings;

internal static class Dialogs
{
    public static ContentDialog Create(string title, object content, string? primary, string close)
    {
        var dialog = new ContentDialog
        {
            Title = title,
            Content = content,
            CloseButtonText = close,
            DefaultButton = primary != null ? ContentDialogButton.Primary : ContentDialogButton.Close,
            XamlRoot = App.Window!.Content.XamlRoot,
            Style = (Style)Application.Current.Resources["DefaultContentDialogStyle"],
            RequestedTheme = (App.Window.Content as FrameworkElement)?.ActualTheme ?? ElementTheme.Default,
        };
        if (primary != null) dialog.PrimaryButtonText = primary;
        return dialog;
    }

    public static async Task ShowMessageAsync(string title, string message)
    {
        await Create(title, new TextBlock { Text = message, TextWrapping = TextWrapping.Wrap }, null, "確定").ShowAsync();
    }

    public static async Task<bool> ConfirmAsync(string title, string message, string action)
    {
        var dialog = Create(title, new TextBlock { Text = message, TextWrapping = TextWrapping.Wrap }, action, "取消");
        return await dialog.ShowAsync() == ContentDialogResult.Primary;
    }

    public static async Task<string?> PickFileAsync(params string[] types)
    {
        var picker = new FileOpenPicker { SuggestedStartLocation = PickerLocationId.DocumentsLibrary };
        foreach (var t in types) picker.FileTypeFilter.Add(t);
        WinRT.Interop.InitializeWithWindow.Initialize(picker, App.Window!.Handle);
        return (await picker.PickSingleFileAsync())?.Path;
    }

    public static async Task<string?> PickSaveFileAsync(string name, string typeName, string extension)
    {
        var picker = new FileSavePicker
        {
            SuggestedStartLocation = PickerLocationId.DocumentsLibrary,
            SuggestedFileName = name,
        };
        picker.FileTypeChoices.Add(typeName, [extension]);
        WinRT.Interop.InitializeWithWindow.Initialize(picker, App.Window!.Handle);
        return (await picker.PickSaveFileAsync())?.Path;
    }
}
