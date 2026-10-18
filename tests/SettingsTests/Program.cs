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
        if (args.Length > 0 && args[0] == "links") return await TestLinks();
        var id = args.Length > 0 ? args[0] : "laya-zh";
        var repo = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", ".."));
        var catalog = args.Length > 1 ? args[1] : Path.Combine(repo, "installer", "models.json");
        var root = Path.Combine(Path.GetTempPath(), "llaime-download-test");
        if (Directory.Exists(root)) Directory.Delete(root, true);
        Directory.CreateDirectory(Path.Combine(root, "install", "models"));
        File.Copy(catalog, Path.Combine(root, "install", "models", "catalog.json"));
        Native.InstallRoot = Path.Combine(root, "install");
        Native.Paths.DataDir = Path.Combine(root, "data");

        var model = ModelCatalog.Load().Single(m => m.Id == id);
        if (!model.Published)
        {
            // Not on the Hub yet: test with a model from a link instead.
            model = (await ModelCatalog.ResolveLinkAsync("https://huggingface.co/unsloth/Qwen3.5-0.8B-GGUF/blob/main/Qwen3.5-0.8B-Q4_K_M.gguf",
                                                         CancellationToken.None)).Single();
        }
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

    // Hugging Face link resolution (network, no large downloads).
    private static async Task<int> TestLinks()
    {
        var root = Path.Combine(Path.GetTempPath(), "llaime-link-test");
        if (Directory.Exists(root)) Directory.Delete(root, true);
        Native.InstallRoot = Path.Combine(root, "install");
        Native.Paths.DataDir = Path.Combine(root, "data");
        var failures = 0;
        void Check(string what, bool ok)
        {
            Console.WriteLine($"  {(ok ? "ok  " : "FAIL")} {what}");
            if (!ok) failures++;
        }
        async Task<List<CatalogModel>> Resolve(string link) => await ModelCatalog.ResolveLinkAsync(link, CancellationToken.None);
        async Task<string> Error(string link)
        {
            try { await Resolve(link); return ""; }
            catch (InvalidDataException e) { return e.Message; }
        }

        var page = await Resolve("https://huggingface.co/unsloth/Qwen3.5-0.8B-GGUF");
        Check($"model page lists {page.Count} GGUF files", page.Count > 5 && page.All(m => m.Kind == "llm"));
        Check("vision projectors (mmproj) are left out", page.All(m => !m.Name.StartsWith("mmproj")));
        Check("each file gets its own folder", page.Select(m => m.Id).Distinct().Count() == page.Count &&
                                                page.Any(m => m.Id == "unsloth-qwen3.5-0.8b-gguf-qwen3.5-0.8b-q4_k_m"));
        var q4 = page.Single(m => m.Name == "Qwen3.5-0.8B-Q4_K_M");
        Check("size and SHA-256 from the Hub", q4.Size == 532517120 &&
              q4.Files[0].Sha256 == "bd258782e35f7f458f8aced1adc053e6e92e89bc735ba3be89d38a06121dc517");
        var blob = await Resolve("https://huggingface.co/unsloth/Qwen3.5-0.8B-GGUF/blob/main/Qwen3.5-0.8B-Q4_K_M.gguf");
        Check("file link picks that file", blob.Count == 1 && blob[0].Id == q4.Id);
        var resolve = await Resolve("https://hf.co/unsloth/Qwen3.5-0.8B-GGUF/resolve/main/Qwen3.5-0.8B-Q4_K_M.gguf?download=true");
        Check("hf.co resolve link with a query", resolve.Count == 1 && resolve[0].Id == q4.Id);
        var shorthand = await Resolve("unsloth/Qwen3.5-0.8B-GGUF");
        Check("owner/name shorthand", shorthand.Count == page.Count);
        Check("not a Hugging Face link", (await Error("https://example.com/x.gguf")).Length > 0);
        Check("no model name", (await Error("https://huggingface.co/unsloth")).Length > 0);
        Check("missing repository", (await Error("https://huggingface.co/llaime-test/does-not-exist")).Contains("找不到"));
        Check("not a GGUF file", (await Error("https://huggingface.co/unsloth/Qwen3.5-0.8B-GGUF/blob/main/README.md")).Contains("GGUF"));

        ModelCatalog.SaveManifest(q4);
        var listed = ModelCatalog.Load();
        Check("a model from a link is listed again", listed.Count == 1 && listed[0].Id == q4.Id && listed[0].Custom && !listed[0].Downloaded);
        ModelCatalog.Delete(q4);
        Check("deleting it removes it from the list", ModelCatalog.Load().Count == 0);

        Directory.Delete(root, true);
        Console.WriteLine(failures == 0 ? "all passed" : $"{failures} failed");
        return failures;
    }
}
