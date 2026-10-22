using System.Diagnostics;
using System.Text.Json;

namespace LLaIME.Settings;

// Updates from the project's GitHub releases, only when asked: reads the latest release, picks
// the installer of this build's variant (LLaIME-<version>-x64.msi, or -moe for builds with the
// Ministry of Education layer), downloads it into %LOCALAPPDATA%\LLaIME\updates checking the
// SHA-256 GitHub publishes for the asset, and starts it. The installer replaces the same
// installation (MajorUpgrade) and closes this app and the engine.
public static class Updater
{
    public const string Repository = "alpha34727/LLaIME";
    public static string ReleasesPage => $"https://github.com/{Repository}/releases";

#if INCLUDE_MOE
    private const string Variant = "-moe";
#else
    private const string Variant = "";
#endif

    public static Version Current { get; } =
        typeof(Updater).Assembly.GetName().Version is { } v ? new Version(v.Major, v.Minor, v.Build) : new Version(0, 0, 0);

    public class Release
    {
        public Version Version { get; init; } = new(0, 0, 0);
        public string Tag { get; init; } = "";
        public string Notes { get; init; } = "";
        public string PageUrl { get; init; } = "";
        // The installer for this build's variant; null when the release has none (then only the
        // release page is offered).
        public CatalogFile? Installer { get; init; }
        public string InstallerUrl { get; init; } = "";
        public bool IsNewer => Version > Current;
    }

    private class ReleaseDocument
    {
        public string Tag_name { get; set; } = "";
        public string? Body { get; set; }  // null when the release has no notes
        public string Html_url { get; set; } = "";
        public bool Draft { get; set; }
        public bool Prerelease { get; set; }
        public List<AssetDocument> Assets { get; set; } = [];
    }

    private class AssetDocument
    {
        public string Name { get; set; } = "";
        public long Size { get; set; }
        public string? Digest { get; set; }  // "sha256:<hex>"
        public string Browser_download_url { get; set; } = "";
    }

    private static readonly JsonSerializerOptions Json = new() { PropertyNameCaseInsensitive = true };

    // The latest published (not draft, not pre-release) release. Throws with a message for
    // people on failure.
    public static async Task<Release> CheckAsync(CancellationToken cancel)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, $"https://api.github.com/repos/{Repository}/releases/latest");
        request.Headers.Accept.ParseAdd("application/vnd.github+json");
        HttpResponseMessage response;
        try
        {
            response = await ModelCatalog.Http.SendAsync(request, cancel);
        }
        catch (HttpRequestException)
        {
            throw new InvalidDataException("無法連到 GitHub，請確認網路連線。");
        }
        using (response)
        {
            if (response.StatusCode == System.Net.HttpStatusCode.NotFound) throw new InvalidDataException("目前還沒有發布的版本。");
            if (!response.IsSuccessStatusCode) throw new InvalidDataException($"GitHub 回應錯誤（{(int)response.StatusCode}），請稍後再試。");
            var doc = JsonSerializer.Deserialize<ReleaseDocument>(await response.Content.ReadAsStringAsync(cancel), Json)
                      ?? throw new InvalidDataException("GitHub 的回應無法解讀。");
            if (!Version.TryParse(doc.Tag_name.TrimStart('v', 'V'), out var version))
                throw new InvalidDataException($"無法解讀版本標籤「{doc.Tag_name}」。");
            version = new Version(version.Major, version.Minor, Math.Max(version.Build, 0));
            var name = $"LLaIME-{version.ToString(3)}-x64{Variant}.msi";
            var asset = doc.Assets.FirstOrDefault(a => a.Name.Equals(name, StringComparison.OrdinalIgnoreCase));
            var sha = asset?.Digest is { } d && d.StartsWith("sha256:", StringComparison.OrdinalIgnoreCase) ? d[7..].ToLowerInvariant() : "";
            return new Release
            {
                Version = version,
                Tag = doc.Tag_name,
                Notes = doc.Body?.Trim() ?? "",
                PageUrl = doc.Html_url.Length > 0 ? doc.Html_url : ReleasesPage,
                // Without a published checksum the file is not run.
                Installer = asset != null && sha.Length == 64 ? new CatalogFile { Name = asset.Name, Size = asset.Size, Sha256 = sha } : null,
                InstallerUrl = asset?.Browser_download_url ?? "",
            };
        }
    }

    private static string Folder => Path.Combine(Native.Paths.DataDir, "updates");

    // Downloads the release's installer (resuming a partial download); returns its path.
    public static async Task<string> DownloadAsync(Release release, IProgress<long> progress, CancellationToken cancel)
    {
        var installer = release.Installer ?? throw new InvalidOperationException("no installer");
        Directory.CreateDirectory(Folder);
        // Installers of other versions are no longer needed.
        foreach (var old in Directory.EnumerateFiles(Folder))
        {
            var oldName = Path.GetFileName(old);
            if (!oldName.StartsWith(installer.Name, StringComparison.OrdinalIgnoreCase))
            {
                try { File.Delete(old); } catch (IOException) { } catch (UnauthorizedAccessException) { }
            }
        }
        var target = Path.Combine(Folder, installer.Name);
        if (File.Exists(target))
        {
            // Downloaded before (e.g. the installer was cancelled): use it if it still matches.
            if (new FileInfo(target).Length == installer.Size)
            {
                await using var stream = File.OpenRead(target);
                var hash = Convert.ToHexString(await System.Security.Cryptography.SHA256.HashDataAsync(stream, cancel)).ToLowerInvariant();
                if (hash == installer.Sha256)
                {
                    progress.Report(installer.Size);
                    return target;
                }
            }
            File.Delete(target);
        }
        await ModelCatalog.DownloadUrlAsync(release.InstallerUrl, installer, target, p => progress.Report(p), cancel);
        return target;
    }

    // Starts the installer with its own window (it asks for administrator rights).
    public static void StartInstaller(string path)
    {
        Process.Start(new ProcessStartInfo("msiexec.exe") { ArgumentList = { "/i", path } });
    }

    public static void OpenPage(string url)
    {
        Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
    }
}
