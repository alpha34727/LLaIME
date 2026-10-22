using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text.Json;

namespace LLaIME.Settings;

// The models the installer does not carry: those in <root>\models\catalog.json (from
// installer/models.json) and any GGUF model (or Laya folder) given by a Hugging Face link.
// They are downloaded into %LOCALAPPDATA%\LLaIME\models\<id>, resuming partial downloads and
// checking each file's size and SHA-256 (from the Hub's LFS metadata). A model added from a
// link keeps its description in model.json next to its files, so it is listed again later.
public class CatalogFile
{
    public string Name { get; set; } = "";  // path in the repository
    public string LocalName => System.IO.Path.GetFileName(Name);
    public long Size { get; set; }
    public string Sha256 { get; set; } = "";
}

public class CatalogModel
{
    public string Id { get; set; } = "";
    public string Kind { get; set; } = "";  // llm, laya
    public string Name { get; set; } = "";
    public string Description { get; set; } = "";
    public string License { get; set; } = "";
    public string Quant { get; set; } = "";  // e.g. Q4_K_M, INT8; empty: read off the file name
    public string Repo { get; set; } = "";
    public string Revision { get; set; } = "main";
    public bool Custom { get; set; }  // added from a link
    public List<CatalogFile> Files { get; set; } = [];

    public bool Published => Repo.Length > 0;
    public long Size => Files.Sum(f => f.Size);
    public string Folder => Path.Combine(Native.Paths.DataDir, "models", Id);
    public bool Downloaded => Files.All(f => File.Exists(Path.Combine(Folder, f.LocalName)) &&
                                            new FileInfo(Path.Combine(Folder, f.LocalName)).Length == f.Size);
    // What engine.ini points at: the (first) GGUF file, or the Laya folder.
    public string EnginePath => Kind == "laya" ? Folder : Path.Combine(Folder, Files[0].LocalName);
}

public static class ModelCatalog
{
    private class CatalogDocument
    {
        public List<CatalogModel> Models { get; set; } = [];
    }

    internal static readonly HttpClient Http = CreateClient();

    private static HttpClient CreateClient()
    {
        var client = new HttpClient { Timeout = Timeout.InfiniteTimeSpan };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("LLaIME-Settings/0.1");
        return client;
    }

    private static readonly JsonSerializerOptions Json = new() { PropertyNameCaseInsensitive = true, WriteIndented = true };
    private const string Manifest = "model.json";

    // The catalog's models, then those added from links (downloaded, or partly).
    public static List<CatalogModel> Load()
    {
        var models = new List<CatalogModel>();
        var path = Path.Combine(Native.InstallRoot, "models", "catalog.json");
        try
        {
            if (File.Exists(path))
            {
                var doc = JsonSerializer.Deserialize<CatalogDocument>(File.ReadAllText(path), Json);
                models.AddRange(doc?.Models.Where(m => m.Files.Count > 0) ?? []);
            }
        }
        catch (JsonException)
        {
        }
        var dir = Path.Combine(Native.Paths.DataDir, "models");
        if (!Directory.Exists(dir)) return models;
        foreach (var manifest in Directory.EnumerateFiles(dir, Manifest, SearchOption.AllDirectories))
        {
            try
            {
                var model = JsonSerializer.Deserialize<CatalogModel>(File.ReadAllText(manifest), Json);
                if (model is { Files.Count: > 0 } && models.All(m => m.Id != model.Id))
                {
                    model.Custom = true;
                    models.Add(model);
                }
            }
            catch (JsonException)
            {
            }
        }
        return models;
    }

    // Records a model added from a link, so it is listed after a restart.
    public static void SaveManifest(CatalogModel model)
    {
        Directory.CreateDirectory(model.Folder);
        File.WriteAllText(Path.Combine(model.Folder, Manifest), JsonSerializer.Serialize(model, Json));
    }

    // --- Hugging Face links -------------------------------------------------------------------

    private class TreeEntry
    {
        public string Type { get; set; } = "";
        public string Path { get; set; } = "";
        public long Size { get; set; }
        public LfsInfo? Lfs { get; set; }
    }

    private class LfsInfo
    {
        public string Oid { get; set; } = "";
        public long Size { get; set; }
    }

    private static readonly System.Text.RegularExpressions.Regex SplitPart =
        new(@"-(\d{5})-of-(\d{5})\.gguf$", System.Text.RegularExpressions.RegexOptions.IgnoreCase);

    // Accepts a model page (https://huggingface.co/owner/name), a file link (.../blob/main/x.gguf
    // or .../resolve/main/x.gguf), hf.co links, or just owner/name. Returns the models it offers:
    // one per GGUF file (a split GGUF counts once, with all its parts), or the Laya ranker if the
    // repository has laya.int8.onnx and tokenizer.bin. Throws with a message for people.
    public static async Task<List<CatalogModel>> ResolveLinkAsync(string link, CancellationToken cancel)
    {
        var (repo, revision, file) = ParseLink(link);
        var url = $"https://huggingface.co/api/models/{repo}/tree/{Uri.EscapeDataString(revision)}?recursive=1";
        using var response = await Http.GetAsync(url, cancel);
        if (response.StatusCode is System.Net.HttpStatusCode.Unauthorized or System.Net.HttpStatusCode.NotFound
            or System.Net.HttpStatusCode.Forbidden)
        {
            throw new InvalidDataException($"找不到「{repo}」，或這個模型需要登入才能下載。");
        }
        response.EnsureSuccessStatusCode();
        var entries = JsonSerializer.Deserialize<List<TreeEntry>>(await response.Content.ReadAsStringAsync(cancel), Json) ?? [];
        var files = entries.Where(e => e.Type == "file").ToList();
        CatalogFile ToFile(TreeEntry e) => new()
        {
            Name = e.Path,
            Size = e.Lfs?.Size ?? e.Size,
            Sha256 = e.Lfs?.Oid ?? "",  // small files outside LFS have only their size checked
        };
        string IdFor(string name) => Sanitize($"{repo.Replace('/', '-')}-{name}");

        var laya = files.FirstOrDefault(e => System.IO.Path.GetFileName(e.Path) == "laya.int8.onnx");
        var tokenizer = files.FirstOrDefault(e => System.IO.Path.GetFileName(e.Path) == "tokenizer.bin");
        // mmproj-*.gguf are vision projectors for multimodal use, not language models.
        var ggufs = files.Where(e => e.Path.EndsWith(".gguf", StringComparison.OrdinalIgnoreCase) &&
                                     !System.IO.Path.GetFileName(e.Path).StartsWith("mmproj", StringComparison.OrdinalIgnoreCase))
                         .ToList();
        if (file != null)
        {
            var picked = files.FirstOrDefault(e => e.Path == file)
                         ?? throw new InvalidDataException($"「{repo}」裡沒有 {file}。");
            if (System.IO.Path.GetFileName(picked.Path) == "laya.int8.onnx" && tokenizer != null) ggufs.Clear();
            else if (!picked.Path.EndsWith(".gguf", StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("只能下載 GGUF 格式的語言模型（.gguf），或 Laya 排序器。");
            else { ggufs = [picked]; laya = null; }
        }

        var models = new List<CatalogModel>();
        if (laya != null && tokenizer != null)
        {
            models.Add(new CatalogModel
            {
                Id = Sanitize($"{repo.Replace('/', '-')}-laya"), Kind = "laya", Name = $"{repo}（Laya）",
                Description = $"從 huggingface.co/{repo} 下載的 Laya 排序器。", Repo = repo, Revision = revision,
                Custom = true, Files = [ToFile(laya), ToFile(tokenizer)],
            });
        }
        foreach (var gguf in ggufs.OrderBy(e => e.Path, StringComparer.OrdinalIgnoreCase))
        {
            var split = SplitPart.Match(gguf.Path);
            if (split.Success && split.Groups[1].Value != "00001") continue;  // listed with part 1
            var parts = split.Success
                ? ggufs.Where(e => e.Path.StartsWith(gguf.Path[..split.Index], StringComparison.Ordinal) && SplitPart.IsMatch(e.Path))
                       .OrderBy(e => e.Path).ToList()
                : [gguf];
            var name = System.IO.Path.GetFileNameWithoutExtension(split.Success ? gguf.Path[..split.Index] : gguf.Path);
            models.Add(new CatalogModel
            {
                Id = IdFor(name), Kind = "llm", Name = name,
                Description = $"從 huggingface.co/{repo} 下載。", Repo = repo, Revision = revision,
                License = "", Custom = true, Files = parts.Select(ToFile).ToList(),
            });
        }
        if (models.Count == 0) throw new InvalidDataException($"「{repo}」裡沒有 GGUF 模型檔（.gguf）。");
        return models;
    }

    private static (string repo, string revision, string? file) ParseLink(string link)
    {
        var text = link.Trim();
        if (!text.Contains("://")) text = "https://huggingface.co/" + text.TrimStart('/');
        if (!Uri.TryCreate(text, UriKind.Absolute, out var uri) ||
            !(uri.Host.Equals("huggingface.co", StringComparison.OrdinalIgnoreCase) ||
              uri.Host.Equals("hf.co", StringComparison.OrdinalIgnoreCase) ||
              uri.Host.Equals("www.huggingface.co", StringComparison.OrdinalIgnoreCase)))
        {
            throw new InvalidDataException("請貼上 huggingface.co 的模型連結，例如 https://huggingface.co/unsloth/Qwen3.5-2B-GGUF");
        }
        var parts = uri.AbsolutePath.Split('/', StringSplitOptions.RemoveEmptyEntries).Select(Uri.UnescapeDataString).ToList();
        if (parts.Count > 0 && parts[0] is "models") parts.RemoveAt(0);
        if (parts.Count > 0 && parts[0] is "datasets" or "spaces") throw new InvalidDataException("這是資料集或 Space 的連結，不是模型。");
        if (parts.Count < 2) throw new InvalidDataException("連結裡要有模型名稱，例如 https://huggingface.co/unsloth/Qwen3.5-2B-GGUF");
        var repo = $"{parts[0]}/{parts[1]}";
        // .../blob/<rev>/<path>, .../resolve/<rev>/<path>, .../tree/<rev>
        if (parts.Count >= 4 && parts[2] is "blob" or "resolve") return (repo, parts[3], parts.Count > 4 ? string.Join('/', parts.Skip(4)) : null);
        if (parts.Count >= 4 && parts[2] == "tree") return (repo, parts[3], null);
        return (repo, "main", null);
    }

    private static string Sanitize(string id)
    {
        var chars = id.ToLowerInvariant().Select(c => char.IsLetterOrDigit(c) || c is '-' or '.' or '_' ? c : '-').ToArray();
        return new string(chars).Trim('-', '.');
    }

    // Downloads every file of `model`; progress reports bytes done of model.Size. Throws on
    // failure (network, size or checksum mismatch); OperationCanceledException when cancelled.
    public static async Task DownloadAsync(CatalogModel model, IProgress<long> progress, CancellationToken cancel)
    {
        Directory.CreateDirectory(model.Folder);
        long before = 0;
        foreach (var file in model.Files)
        {
            var target = Path.Combine(model.Folder, file.LocalName);
            if (File.Exists(target) && new FileInfo(target).Length == file.Size)
            {
                before += file.Size;
                progress.Report(before);
                continue;
            }
            var done = before;
            await DownloadFileAsync(model.Repo, model.Revision, file, target, p => progress.Report(done + p), cancel);
            before += file.Size;
        }
    }

    private static Task DownloadFileAsync(string repo, string revision, CatalogFile file, string target,
                                          Action<long> progress, CancellationToken cancel)
    {
        var path = string.Join('/', file.Name.Split('/').Select(Uri.EscapeDataString));
        var url = $"https://huggingface.co/{repo}/resolve/{Uri.EscapeDataString(revision)}/{path}";
        return DownloadUrlAsync(url, file, target, progress, cancel);
    }

    // Downloads `url` into `target` through target.part, resuming a partial download; checks the
    // size and, when given, the SHA-256 of `file`. Also used for installer updates.
    public static async Task DownloadUrlAsync(string url, CatalogFile file, string target,
                                              Action<long> progress, CancellationToken cancel)
    {
        var part = target + ".part";
        using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        long have = File.Exists(part) ? new FileInfo(part).Length : 0;
        if (have > file.Size) { File.Delete(part); have = 0; }
        if (have > 0)
        {
            // Resume: hash what is already there.
            await using var existing = File.OpenRead(part);
            var buffer = new byte[1 << 20];
            int n;
            while ((n = await existing.ReadAsync(buffer, cancel)) > 0) sha.AppendData(buffer, 0, n);
        }

        if (have < file.Size)
        {
            using var request = new HttpRequestMessage(HttpMethod.Get, url);
            if (have > 0) request.Headers.Range = new RangeHeaderValue(have, null);
            using var response = await Http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, cancel);
            if (have > 0 && response.StatusCode != System.Net.HttpStatusCode.PartialContent)
            {
                // The server ignored the range: start over.
                have = 0;
                sha.GetHashAndReset();
            }
            response.EnsureSuccessStatusCode();
            await using var output = new FileStream(part, have > 0 ? FileMode.Append : FileMode.Create,
                                                    FileAccess.Write, FileShare.None, 1 << 16, useAsync: true);
            await using var input = await response.Content.ReadAsStreamAsync(cancel);
            var buffer = new byte[1 << 20];
            int n;
            var lastReport = DateTime.MinValue;
            while ((n = await input.ReadAsync(buffer, cancel)) > 0)
            {
                await output.WriteAsync(buffer.AsMemory(0, n), cancel);
                sha.AppendData(buffer, 0, n);
                have += n;
                if (have > file.Size) throw new InvalidDataException($"{file.Name} 比預期大");
                if ((DateTime.UtcNow - lastReport).TotalMilliseconds > 200)
                {
                    progress(have);
                    lastReport = DateTime.UtcNow;
                }
            }
        }
        progress(have);
        if (have != file.Size) throw new InvalidDataException($"{file.Name} 下載不完整（{have:N0} / {file.Size:N0} 位元組）");
        var hash = Convert.ToHexString(sha.GetHashAndReset()).ToLowerInvariant();
        if (file.Sha256.Length > 0 && hash != file.Sha256)
        {
            File.Delete(part);
            throw new InvalidDataException($"{file.Name} 的檢查碼不符，已刪除，請重新下載");
        }
        File.Move(part, target, overwrite: true);
    }

    public static void Delete(CatalogModel model)
    {
        if (Directory.Exists(model.Folder)) Directory.Delete(model.Folder, recursive: true);
    }
}
