namespace LLaIME.Settings;

// Stands in for the native bridge: where the catalog is and where models go.
public class InitResult
{
    public string DataDir { get; set; } = "";
}

public static class Native
{
    public static string InstallRoot { get; set; } = "";
    public static InitResult Paths { get; } = new();
}

public static class Program
{
    public static async Task<int> Main(string[] args)
    {
        var id = args.Length > 0 ? args[0] : "qwen3.5-0.8b";
        var repo = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", ".."));
        var catalog = args.Length > 1 ? args[1] : Path.Combine(repo, "installer", "models.json");
        var root = Path.Combine(Path.GetTempPath(), "llaime-download-test");
        if (Directory.Exists(root)) Directory.Delete(root, true);
        Directory.CreateDirectory(Path.Combine(root, "install", "models"));
        File.Copy(catalog, Path.Combine(root, "install", "models", "catalog.json"));
        Native.InstallRoot = Path.Combine(root, "install");
        Native.Paths.DataDir = Path.Combine(root, "data");

        var model = ModelCatalog.Load().Single(m => m.Id == id);
        Console.WriteLine($"{model.Name}: {model.Size:N0} bytes from {model.Repo}");
        var failures = 0;
        void Check(string what, bool ok)
        {
            Console.WriteLine($"  {(ok ? "ok  " : "FAIL")} {what}");
            if (!ok) failures++;
        }

        // 1. Cancel at about 30%: the partial file stays.
        using (var cancel = new CancellationTokenSource())
        {
            long last = 0;
            var progress = new Progress<long>(done =>
            {
                last = done;
                if (done > model.Size * 3 / 10) cancel.Cancel();
            });
            try
            {
                await ModelCatalog.DownloadAsync(model, progress, cancel.Token);
                Check("cancelled", false);
            }
            catch (OperationCanceledException)
            {
                var part = Path.Combine(model.Folder, model.Files[0].Name + ".part");
                var have = File.Exists(part) ? new FileInfo(part).Length : 0;
                Check($"cancelled with {have:N0} bytes kept", have > 0 && have < model.Size);
            }
        }

        // 2. Resume to the end; the hash covers both parts.
        var started = DateTime.UtcNow;
        await ModelCatalog.DownloadAsync(model, new Progress<long>(_ => { }), CancellationToken.None);
        Console.WriteLine($"  resumed in {(DateTime.UtcNow - started).TotalSeconds:0} s");
        Check("downloaded and verified", model.Downloaded);
        Check("no partial file left", !Directory.EnumerateFiles(model.Folder, "*.part").Any());

        // 3. A corrupted partial file is caught by the checksum and deleted.
        var target = Path.Combine(model.Folder, model.Files[0].Name);
        File.Move(target, target + ".part");
        using (var f = new FileStream(target + ".part", FileMode.Open)) { f.Position = 1000; f.WriteByte(0x55); f.WriteByte(0xAA); }
        try
        {
            await ModelCatalog.DownloadAsync(model, new Progress<long>(_ => { }), CancellationToken.None);
            Check("corruption detected", false);
        }
        catch (InvalidDataException e)
        {
            Check($"corruption detected: {e.Message}", !File.Exists(target + ".part") && !File.Exists(target));
        }

        Directory.Delete(root, true);
        Console.WriteLine(failures == 0 ? "all passed" : $"{failures} failed");
        return failures;
    }
}
