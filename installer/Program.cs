using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

internal sealed class Installer : Form
{
    readonly TextBox iso = new TextBox();
    readonly TextBox destination = new TextBox();
    readonly Button install = new Button();
    readonly Button browse = new Button();
    readonly CheckBox reuse = new CheckBox();
    readonly Label status = new Label();
    readonly ProgressBar progress = new ProgressBar();
    readonly string logDirectory;
    bool running, complete;

    [DllImport("msi.dll", CharSet = CharSet.Unicode)] static extern uint MsiOpenPackage(string path, out uint handle);
    [DllImport("msi.dll", CharSet = CharSet.Unicode)] static extern uint MsiGetProperty(uint handle, string name, StringBuilder value, ref uint length);
    [DllImport("msi.dll", CharSet = CharSet.Unicode)] static extern int MsiQueryProductState(string product);
    [DllImport("msi.dll")] static extern uint MsiCloseHandle(uint handle);

    static bool IsInstalled(string package)
    {
        uint handle;
        if (MsiOpenPackage(package, out handle) != 0) throw new Exception("Cannot read the installer package.");
        try {
            var code = new StringBuilder(40); uint length = 40;
            if (MsiGetProperty(handle, "ProductCode", code, ref length) != 0) throw new Exception("Cannot identify the installer package.");
            return MsiQueryProductState(code.ToString()) == 5;
        } finally { MsiCloseHandle(handle); }
    }

    [STAThread]
    static int Main(string[] args)
    {
        try {
            using (var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("payload.zip"))
            using (var archive = new ZipArchive(stream, ZipArchiveMode.Read)) {
                if (archive.GetEntry("HazeRecomp.msi") == null || archive.GetEntry("scripts/setup.ps1") == null ||
                    archive.GetEntry("tools/7zip/7z.exe") == null) throw new Exception("Incomplete installer payload.");
                if (args.Length == 1 && args[0] == "--verify-payload") {
                    foreach (var entry in archive.Entries) using (var input = entry.Open()) input.CopyTo(Stream.Null);
                    return 0;
                }
            }
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new Installer());
            return 0;
        } catch (Exception e) {
            if (args.Length == 1 && args[0] == "--verify-payload") return 1;
            MessageBox.Show(e.Message, "Haze installation failed"); return 1;
        }
    }

    Installer()
    {
        Text = "Install Haze Recomp";
        ClientSize = new Size(670, 415);
        Font = new Font("Segoe UI", 10);
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = Color.White;
        var heading = new Label { Text = "Install Haze Recomp", Left = 28, Top = 24, Width = 610, Height = 38,
            Font = new Font("Segoe UI", 19, FontStyle.Bold) };
        var description = new Label { Left = 30, Top = 72, Width = 610, Height = 58,
            Text = "Choose your disc-decrypted Haze USA ISO. The installer downloads RPCS3\nand PS3 firmware from their official providers, prepares your game, and\ninstalls a ready-to-play launcher. Internet access and 12 GB free are required." };
        Controls.Add(heading); Controls.Add(description);
        Controls.Add(new Label { Text = "Haze disc image (BLUS30094)", Left = 30, Top = 148, Width = 500 });
        iso.SetBounds(30, 173, 505, 28);
        browse.Text = "Browse..."; browse.SetBounds(548, 171, 90, 30);
        browse.Click += delegate {
            using (var dialog = new OpenFileDialog { Title = "Choose your Haze USA ISO", Filter = "PS3 disc image (*.iso)|*.iso" })
                if (dialog.ShowDialog(this) == DialogResult.OK) iso.Text = dialog.FileName;
        };
        Controls.Add(iso); Controls.Add(browse);
        Controls.Add(new Label { Text = "Install location", Left = 30, Top = 214, Width = 500 });
        destination.Text = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs", "Haze Recomp");
        using (var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey("Software\\HazeRecomp")) {
            string previous = key == null ? null : key.GetValue("InstallFolder") as string;
            if (!String.IsNullOrEmpty(previous)) destination.Text = previous;
        }
        destination.SetBounds(30, 240, 608, 28); Controls.Add(destination);
        reuse.Text = "Keep my existing prepared game and firmware";
        reuse.SetBounds(30, 276, 605, 25);
        reuse.Checked = File.Exists(Path.Combine(destination.Text, "launcher.ini"));
        Controls.Add(reuse);
        status.SetBounds(30, 309, 608, 36); status.Text = "Your saves and customized settings are preserved when upgrading.";
        Controls.Add(status);
        progress.SetBounds(30, 360, 465, 22); Controls.Add(progress);
        install.Text = "Install"; install.SetBounds(528, 350, 110, 38); install.Click += BeginInstall; Controls.Add(install);
        logDirectory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Haze Recomp", "InstallerLogs", DateTime.UtcNow.ToString("yyyyMMdd-HHmmss") + "-" + Guid.NewGuid().ToString("N"));
        FormClosing += delegate(object sender, FormClosingEventArgs e) { if (running) e.Cancel = true; };
    }

    static string Quote(string value)
    {
        if (value.IndexOf('"') >= 0 || value.IndexOf('\r') >= 0 || value.IndexOf('\n') >= 0)
            throw new Exception("Unsupported path.");
        return "\"" + value.TrimEnd('\\') + "\"";
    }

    void Report(string message) { BeginInvoke(new Action(delegate { status.Text = message; })); }

    int Run(string file, string arguments, string log)
    {
        using (var writer = new StreamWriter(Path.Combine(logDirectory, log), true))
        using (var process = new Process()) {
            process.StartInfo = new ProcessStartInfo(file, arguments) { UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true };
            object guard = new object();
            process.OutputDataReceived += delegate(object sender, DataReceivedEventArgs e) {
                if (e.Data != null) { lock (guard) writer.WriteLine(e.Data); if (e.Data.EndsWith("...")) Report(e.Data); }
            };
            process.ErrorDataReceived += delegate(object sender, DataReceivedEventArgs e) { if (e.Data != null) lock (guard) writer.WriteLine(e.Data); };
            process.Start(); process.BeginOutputReadLine(); process.BeginErrorReadLine();
            if (!process.WaitForExit(60 * 60 * 1000)) { process.Kill(); process.WaitForExit(); throw new Exception("Installation timed out. See logs: " + logDirectory); }
            process.WaitForExit(); return process.ExitCode;
        }
    }

    async void BeginInstall(object sender, EventArgs e)
    {
        if (complete) { Process.Start(new ProcessStartInfo(Path.Combine(destination.Text, "HazeLauncher.exe")) { WorkingDirectory = destination.Text }); Close(); return; }
        string isoPath = iso.Text, target;
        bool keep = reuse.Checked;
        try {
            target = Path.GetFullPath(destination.Text);
            if (target.TrimEnd('\\') == Path.GetPathRoot(target).TrimEnd('\\')) throw new Exception("Choose an application folder, not a drive root.");
            Quote(target); Quote(isoPath);
            if (!keep && !File.Exists(isoPath)) throw new Exception("Choose your Haze USA ISO first.");
            if (keep && !File.Exists(Path.Combine(target, "launcher.ini"))) throw new Exception("No existing game configuration at this location. Choose an ISO for first installation.");
        } catch (Exception error) { MessageBox.Show(this, error.Message, Text); return; }
        running = true; install.Enabled = browse.Enabled = iso.Enabled = destination.Enabled = reuse.Enabled = false;
        progress.Style = ProgressBarStyle.Marquee;
        string temporary = Path.Combine(Path.GetTempPath(), "HazeSetup-" + Guid.NewGuid().ToString("N"));
        try {
            Directory.CreateDirectory(logDirectory);
            await Task.Run(delegate {
                Report("Preparing the installer...");
                Directory.CreateDirectory(temporary);
                using (var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("payload.zip"))
                using (var archive = new ZipArchive(stream, ZipArchiveMode.Read)) archive.ExtractToDirectory(temporary);
                // Windows Installer needs the original embedded cabinet for repair.
                // Keep only the MSI in its package cache; setup tools remain temporary.
                string extracted = Path.Combine(temporary, "HazeRecomp.msi"), hash;
                using (var sha = SHA256.Create())
                using (var stream = File.OpenRead(extracted)) hash = BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "");
                string cache = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Haze Recomp", "InstallerCache", hash);
                Directory.CreateDirectory(cache);
                string package = Path.Combine(cache, "HazeRecomp.msi");
                File.Copy(extracted, package, true);
                bool repair = IsInstalled(package);
                Report("Installing the launcher and game files...");
                var system = Environment.GetFolderPath(Environment.SpecialFolder.System);
                int result = Run(Path.Combine(system, "msiexec.exe"), "/i " + Quote(package) +
                    (repair ? " REINSTALL=ALL REINSTALLMODE=vamus" : "") +
                    " /qn /norestart INSTALLFOLDER=" + Quote(target) + " /l*v " + Quote(Path.Combine(logDirectory, "msi.log")), "msi-process.log");
                if (result != 0 && result != 3010) throw new Exception("Windows Installer failed (" + result + ").");
                string defaults = Path.Combine(target, "defaults", "config");
                string settings = Path.Combine(target, "config");
                Directory.CreateDirectory(settings);
                foreach (string file in Directory.GetFiles(defaults, "*.ini")) {
                    string setting = Path.Combine(settings, Path.GetFileName(file));
                    if (!File.Exists(setting)) File.Copy(file, setting);
                }
                if (!keep) {
                    Report("Preparing your game and downloading firmware...");
                    result = Run(Path.Combine(system, "WindowsPowerShell/v1.0/powershell.exe"),
                        "-NoProfile -ExecutionPolicy Bypass -File " + Quote(Path.Combine(temporary, "scripts/setup.ps1")) +
                        " -Iso " + Quote(isoPath) + " -InstallDirectory " + Quote(target) + " -DownloadFirmware -NoGui", "setup.log");
                    if (result != 0) throw new Exception("Game preparation failed. Your previous configuration and saves were kept. Run this installer to retry.");
                }
                result = Run(Path.Combine(target, "HazeLauncher.exe"), "--check --no-ui", "launcher-check.log");
                if (result != 0) throw new Exception("The launcher could not validate the prepared game. Run the installer again with your ISO.");
            });
            destination.Text = target;
            status.Text = "Installation complete. Haze is ready to play.";
            progress.Style = ProgressBarStyle.Continuous; progress.Value = 100;
            complete = true; install.Text = "Play Haze";
        } catch (Exception error) {
            status.Text = "Installation did not complete. You can retry with the same ISO.";
            progress.Style = ProgressBarStyle.Continuous;
            MessageBox.Show(this, error.Message + "\n\nInstaller logs: " + logDirectory, "Installation failed", MessageBoxButtons.OK, MessageBoxIcon.Error);
            browse.Enabled = iso.Enabled = destination.Enabled = reuse.Enabled = true;
        } finally {
            running = false; install.Enabled = true;
            // Only remove the unique directory created by this installer instance.
            try { if (Directory.Exists(temporary)) Directory.Delete(temporary, true); } catch { }
        }
    }
}
