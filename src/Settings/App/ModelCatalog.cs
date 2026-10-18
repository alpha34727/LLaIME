using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text.Json;

namespace LLaIME.Settings;

// The models the installer does not carry (<root>\models\catalog.json, from
// installer/models.json): downloaded from Hugging Face into %LOCALAPPDATA%\LLaIME\models\<id>,
// resuming partial downloads, and checked against each file's size and SHA-256.
public class CatalogFile
{
    public string Name { get; set; } = "";
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
    public string Repo { get; set; } = "";
    public List<CatalogFile> Files { get; set; } = [];

    public bool Published => Repo.Length > 0;
    public long Size => Files.Sum(f => f.Size);
    public string Folder => Path.Combine(Native.Paths.DataDir, "models", Id);
    public bool Downloaded => Files.All(f => File.Exists(Path.Combine(Folder, f.Name)) &&
                                            new FileInfo(Path.Combine(Folder, f.Name)).Length == f.Size);
    // What engine.ini points at: the GGUF file, or the Laya folder.
    public string EnginePath => Kind == "laya" ? Folder : Path.Combine(Folder, Files[0].Name);
}

public static class ModelCatalog
{
    private class CatalogDocument
    {
        public List<CatalogModel> Models { get; set; } = [];
    }

    private static readonly HttpClient Http = CreateClient();

    private static HttpClient CreateClient()
    {
        var client = new HttpClient { Timeout = Timeout.InfiniteTimeSpan };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("LLaIME-Settings/0.1");
        return client;
    }

    public static List<CatalogModel> Load()
    {
        var path = Path.Combine(Native.InstallRoot, "models", "catalog.json");
        if (!File.Exists(path)) return [];
        try
        {
            var doc = JsonSerializer.Deserialize<CatalogDocument>(File.ReadAllText(path),
                new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
            return doc?.Models.Where(m => m.Files.Count > 0).ToList() ?? [];
        }
        catch (JsonException)
        {
            return [];
        }
    }

    // Downloads every file of `model`; progress reports bytes done of model.Size. Throws on
    // failure (network, size or checksum mismatch); OperationCanceledException when cancelled.
    public static async Task DownloadAsync(CatalogModel model, IProgress<long> progress, CancellationToken cancel)
    {
        Directory.CreateDirectory(model.Folder);
        long before = 0;
        foreach (var file in model.Files)
        {
            var target = Path.Combine(model.Folder, file.Name);
            if (File.Exists(target) && new FileInfo(target).Length == file.Size)
            {
                before += file.Size;
                progress.Report(before);
                continue;
            }
            var done = before;
            await DownloadFileAsync(model.Repo, file, target, p => progress.Report(done + p), cancel);
            before += file.Size;
        }
    }

    private static async Task DownloadFileAsync(string repo, CatalogFile file, string target,
                                                Action<long> progress, CancellationToken cancel)
    {
        var part = target + ".part";
        var url = $"https://huggingface.co/{repo}/resolve/main/{Uri.EscapeDataString(file.Name)}";
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
        if (hash != file.Sha256)
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
