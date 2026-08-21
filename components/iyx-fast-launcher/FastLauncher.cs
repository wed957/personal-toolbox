using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Management;
using System.Net;
using System.Net.WebSockets;
using System.Reflection;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;
using System.Windows.Forms;

[assembly: AssemblyTitle("IYX绿色版")]
[assembly: AssemblyDescription("IYX 驱动界面绿色版")]
[assembly: AssemblyCompany("IYX Green")]
[assembly: AssemblyProduct("IYX绿色版")]
[assembly: AssemblyVersion("3.1.0.0")]
[assembly: AssemblyFileVersion("3.1.0.0")]

internal static class FastProgram
{
    private const string PayloadResource = "IYX.Payload.zip";
    private const string SdkResource = "IYX.Sdk.js";
    private const string InstanceMutexName = "Local\\IYXFastLauncher.SingleInstance";
    private static readonly object extractionLock = new object();

    [STAThread]
    private static void Main(string[] args)
    {
        if (args.Length == 1
            && string.Equals(args[0], "--verify-patches", StringComparison.Ordinal))
        {
            VerifyPayloadPatches();
            return;
        }

        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);

        bool ownsInstance;
        using (Mutex instanceMutex = new Mutex(true, InstanceMutexName, out ownsInstance))
        {
            if (!ownsInstance)
            {
                MessageBox.Show(
                    "IYX绿色版已经在运行。",
                    "IYX绿色版",
                    MessageBoxButtons.OK,
                    MessageBoxIcon.Information);
                return;
            }

            RunLauncher();
        }
    }

    private static void RunLauncher()
    {
        string root = Path.Combine(
            Path.GetTempPath(),
            "IYXFast_" + Process.GetCurrentProcess().Id + "_" + Guid.NewGuid().ToString("N"));
        string stateRoot = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "IYXFastLauncher");
        Process service = null;
        Process iyx = null;
        Process edge = null;
        PayloadServer server = null;
        BrowserBridge bridge = null;

        try
        {
            StopStaleDriverServices(stateRoot);
            Directory.CreateDirectory(root);

            server = new PayloadServer(root);
            server.Start();
            edge = StartEdge(server.Port, root);

            PrepareBootFiles(root, stateRoot);
            string dataRoot = Path.Combine(stateRoot, "Data");
            string localData = Path.Combine(dataRoot, "Local");
            string roamingData = Path.Combine(dataRoot, "Roaming");
            string tempData = Path.Combine(root, "Data", "Temp");
            Directory.CreateDirectory(roamingData);
            Directory.CreateDirectory(tempData);

            string servicePath = Path.Combine(
                localData, "IYXAST", "apps", "driver_service", "driver.service.exe");
            service = StartChild(servicePath, Path.GetDirectoryName(servicePath), localData, roamingData, tempData);

            string iyxPath = Path.Combine(root, "IYX.exe");
            iyx = StartChild(iyxPath, root, localData, roamingData, tempData);

            if (!WaitForPort(7678, 1500))
                throw new InvalidOperationException("驱动服务启动超时，端口 7678 未打开。");

            bridge = new BrowserBridge();
            bridge.Connect();
            WaitForLifetime(edge, edge == null ? null : GetEdgeProfile(root), iyx);
        }
        catch (Exception ex)
        {
            MessageBox.Show(
                "启动失败：\r\n" + ex.Message,
                "IYX绿色版",
                MessageBoxButtons.OK,
                MessageBoxIcon.Error);
        }
        finally
        {
            if (service != null && !service.HasExited)
                TryResetDriverState();
            if (server != null)
                server.Dispose();
            if (bridge != null)
                bridge.Dispose();
            StopProcess(edge);
            StopProcess(iyx);
            StopProcess(service);
            TryDeleteDirectory(root);
        }
    }

    private static void VerifyPayloadPatches()
    {
        string verificationRoot = Path.Combine(
            Path.GetTempPath(),
            "IYXVerify_" + Process.GetCurrentProcess().Id + "_" + Guid.NewGuid().ToString("N"));
        try
        {
            using (PayloadServer server = new PayloadServer(Path.GetTempPath()))
            {
            }

            string bootRoot = Path.Combine(verificationRoot, "Boot");
            string stateRoot = Path.Combine(verificationRoot, "State");
            PrepareBootFiles(bootRoot, stateRoot, false);

            string magnet = Path.Combine(
                stateRoot,
                "Data", "Local", "IYXAST", "apps", "driver_service", "dlls", "magnet0.dll");
            using (FileStream locked = new FileStream(
                magnet, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                PrepareBootFiles(bootRoot, stateRoot, false);
            }
        }
        catch
        {
            Environment.ExitCode = 1;
        }
        finally
        {
            TryDeleteDirectory(verificationRoot);
        }
    }

    private static string GetEdgeProfile(string root)
    {
        return Path.Combine(root, "EdgeProfile");
    }

    private static void PrepareBootFiles(
        string root, string stateRoot, bool includeBrowserBundle = true)
    {
        string localRoot = Path.Combine(stateRoot, "Data", "Local", "IYXAST", "apps");
        string servicePrefix = "Data/Local/IYXAST/apps/driver_service/";
        string scriptIni = "Data/Local/IYXAST/apps/driver_script/app.ini";
        string scriptIndex = "Data/Local/IYXAST/apps/driver_script/index.html";
        string browserPrefix = "browser/";
        string embedRoamingPrefix = "Data/Roaming/iyx_ast_embed/";

        ExtractEntries(root, delegate (string name)
        {
            return string.Equals(name, "IYX.exe", StringComparison.OrdinalIgnoreCase)
                || string.Equals(name, "launch.ini", StringComparison.OrdinalIgnoreCase);
        }, null);

        ExtractEntries(stateRoot, delegate (string name)
        {
            return string.Equals(name, scriptIni, StringComparison.OrdinalIgnoreCase)
                || string.Equals(name, scriptIndex, StringComparison.OrdinalIgnoreCase)
                || name.StartsWith(servicePrefix, StringComparison.OrdinalIgnoreCase)
                || (includeBrowserBundle
                    && (name.StartsWith(browserPrefix, StringComparison.OrdinalIgnoreCase)
                        || name.StartsWith(embedRoamingPrefix,
                                           StringComparison.OrdinalIgnoreCase)));
        }, IsPersistentDriverConfig);

        Directory.CreateDirectory(localRoot);
        if (includeBrowserBundle)
            LinkBrowserBundle(root, Path.Combine(stateRoot, "browser"));
    }

    // driver.service preflights <launcher root>\browser\iyx_ast_embed.exe for the
    // magnet check window. The bundle is large, so it is extracted once into the
    // persistent state root and junctioned into each per-run root.
    private static void LinkBrowserBundle(string root, string persistentBrowser)
    {
        string link = Path.Combine(root, "browser");
        if (Directory.Exists(link) || !Directory.Exists(persistentBrowser))
            return;

        ProcessStartInfo info = new ProcessStartInfo();
        info.FileName = "cmd.exe";
        info.Arguments = "/c mklink /J \"" + link + "\" \"" + persistentBrowser + "\"";
        info.UseShellExecute = false;
        info.CreateNoWindow = true;
        using (Process process = Process.Start(info))
        {
            if (process != null)
                process.WaitForExit(5000);
        }
        if (!Directory.Exists(link))
            throw new InvalidOperationException("无法链接磁轴检测组件目录（mklink /J 失败）。");
    }

    private static bool IsPersistentDriverConfig(string name)
    {
        const string serviceRoot = "Data/Local/IYXAST/apps/driver_service/";
        return string.Equals(
                name,
                serviceRoot + "user.list.cfg.json",
                StringComparison.OrdinalIgnoreCase)
            || name.StartsWith(serviceRoot + "user_cfgs/", StringComparison.OrdinalIgnoreCase)
            || name.StartsWith("Data/Roaming/iyx_ast_embed/",
                               StringComparison.OrdinalIgnoreCase);
    }

    private static void ExtractEntries(
        string root,
        Func<string, bool> shouldExtract,
        Func<string, bool> preserveExisting)
    {
        using (Stream payload = Assembly.GetExecutingAssembly().GetManifestResourceStream(PayloadResource))
        {
            if (payload == null)
                throw new InvalidOperationException("内置程序文件不存在。");

            using (ZipArchive archive = new ZipArchive(payload, ZipArchiveMode.Read, false))
            {
                foreach (ZipArchiveEntry entry in archive.Entries)
                {
                    string name = entry.FullName.Replace('\\', '/');
                    if (entry.Name.Length == 0 || !shouldExtract(name))
                        continue;

                    string target = Path.Combine(root, name.Replace('/', Path.DirectorySeparatorChar));
                    if (preserveExisting != null && preserveExisting(name) && File.Exists(target))
                        continue;
                    if (CanReuseExistingFile(target, entry.Length))
                        continue;

                    string parent = Path.GetDirectoryName(target);
                    if (!string.IsNullOrEmpty(parent))
                        Directory.CreateDirectory(parent);

                    WriteEntryWithRetry(entry, target);
                }
            }
        }
    }

    private static bool CanReuseExistingFile(string path, long expectedLength)
    {
        try
        {
            return File.Exists(path) && new FileInfo(path).Length == expectedLength;
        }
        catch
        {
            return false;
        }
    }

    private static void WriteEntryWithRetry(ZipArchiveEntry entry, string target)
    {
        Exception lastError = null;
        for (int attempt = 0; attempt < 20; attempt++)
        {
            if (CanReuseExistingFile(target, entry.Length))
                return;

            try
            {
                using (Stream input = entry.Open())
                using (FileStream output = new FileStream(
                    target,
                    FileMode.Create,
                    FileAccess.Write,
                    FileShare.None,
                    1024 * 1024,
                    FileOptions.SequentialScan))
                {
                    input.CopyTo(output, 1024 * 1024);
                }
                return;
            }
            catch (IOException ex)
            {
                lastError = ex;
            }
            catch (UnauthorizedAccessException ex)
            {
                lastError = ex;
            }

            Thread.Sleep(100);
        }

        throw new IOException("无法更新启动文件：" + target, lastError);
    }

    private static void StopStaleDriverServices(string stateRoot)
    {
        string serviceDirectory = Path.GetFullPath(Path.Combine(
            stateRoot, "Data", "Local", "IYXAST", "apps", "driver_service"))
            .TrimEnd(Path.DirectorySeparatorChar)
            + Path.DirectorySeparatorChar;

        try
        {
            using (ManagementObjectSearcher searcher = new ManagementObjectSearcher(
                "SELECT ProcessId, ExecutablePath FROM Win32_Process WHERE Name='driver.service.exe'"))
            using (ManagementObjectCollection processes = searcher.Get())
            {
                foreach (ManagementObject item in processes)
                {
                    try
                    {
                        string executablePath = item["ExecutablePath"] as string;
                        if (string.IsNullOrEmpty(executablePath)
                            || !Path.GetFullPath(executablePath).StartsWith(
                                serviceDirectory, StringComparison.OrdinalIgnoreCase))
                            continue;

                        int processId = Convert.ToInt32(item["ProcessId"]);
                        using (Process stale = Process.GetProcessById(processId))
                        {
                            stale.Kill();
                            stale.WaitForExit(3000);
                        }
                    }
                    catch
                    {
                    }
                }
            }
        }
        catch
        {
        }
    }

    internal static string ExtractKeyboardTool(string root)
    {
        string target = Path.Combine(root, "tools", "keyboard-check.exe");
        if (File.Exists(target))
            return target;

        lock (extractionLock)
        {
            if (File.Exists(target))
                return target;

            Directory.CreateDirectory(Path.GetDirectoryName(target));
            using (Stream payload = Assembly.GetExecutingAssembly().GetManifestResourceStream(PayloadResource))
            using (ZipArchive archive = new ZipArchive(payload, ZipArchiveMode.Read, false))
            {
                ZipArchiveEntry entry = archive.GetEntry("tools/keyboard-check.exe");
                if (entry == null)
                    throw new InvalidOperationException("键盘检查工具不存在。");

                using (Stream input = entry.Open())
                using (FileStream output = new FileStream(target, FileMode.Create, FileAccess.Write, FileShare.None))
                {
                    input.CopyTo(output, 1024 * 1024);
                }
            }
        }

        return target;
    }

    private static Process StartChild(
        string executable,
        string workingDirectory,
        string localData,
        string roamingData,
        string tempData)
    {
        ProcessStartInfo info = new ProcessStartInfo();
        info.FileName = executable;
        info.WorkingDirectory = workingDirectory;
        info.UseShellExecute = false;
        info.EnvironmentVariables["LOCALAPPDATA"] = localData;
        info.EnvironmentVariables["APPDATA"] = roamingData;
        info.EnvironmentVariables["TEMP"] = tempData;
        info.EnvironmentVariables["TMP"] = tempData;
        Process process = Process.Start(info);
        if (process == null)
            throw new InvalidOperationException("无法启动：" + Path.GetFileName(executable));
        return process;
    }

    private static Process StartEdge(int port, string root)
    {
        string edgePath = FindEdgePath();
        string profile = GetEdgeProfile(root);
        string url = "http://127.0.0.1:" + port + "/driver_script/index.html";

        ProcessStartInfo info = new ProcessStartInfo();
        info.FileName = edgePath;
        info.Arguments = "--app=" + url
            + " --user-data-dir=\"" + profile + "\""
            + " --user-agent=\"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 Chrome/128.0.0.0 Safari/537.36 Electron/32.0.0 cherry_ast_embed/1.0.0\""
            + " --no-first-run --no-default-browser-check"
            + " --disable-extensions --disable-sync --disable-background-networking"
            + " --window-size=1600,900";
        info.UseShellExecute = false;
        info.CreateNoWindow = true;
        Process process = Process.Start(info);
        if (process == null)
            throw new InvalidOperationException("无法启动 Microsoft Edge。");
        return process;
    }

    private static string FindEdgePath()
    {
        string[] candidates = new string[]
        {
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Microsoft", "Edge", "Application", "msedge.exe"),
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "Microsoft", "Edge", "Application", "msedge.exe")
        };

        foreach (string candidate in candidates)
        {
            if (File.Exists(candidate))
                return candidate;
        }

        throw new FileNotFoundException("未找到 Microsoft Edge 浏览器。");
    }

    private static bool WaitForPort(int port, int timeoutMs)
    {
        DateTime deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
        while (DateTime.UtcNow < deadline)
        {
            try
            {
                using (System.Net.Sockets.TcpClient client = new System.Net.Sockets.TcpClient())
                {
                    IAsyncResult result = client.BeginConnect("127.0.0.1", port, null, null);
                    if (result.AsyncWaitHandle.WaitOne(80) && client.Connected)
                    {
                        client.EndConnect(result);
                        return true;
                    }
                }
            }
            catch
            {
            }

            Thread.Sleep(15);
        }

        return false;
    }

    private static void TryResetDriverState()
    {
        try
        {
            using (ClientWebSocket socket = new ClientWebSocket())
            {
                using (CancellationTokenSource connectTimeout = new CancellationTokenSource(1600))
                {
                    socket.ConnectAsync(new Uri("ws://127.0.0.1:8083/entry"), connectTimeout.Token)
                        .GetAwaiter().GetResult();
                }

                string calibrationId = "iyx-exit-calibration-" + Guid.NewGuid().ToString("N");
                string watchId = "iyx-exit-watch-" + Guid.NewGuid().ToString("N");
                SendDriverRequestAndWait(
                    socket,
                    calibrationId,
                    51,
                    "{\"Value\":false}");
                SendDriverRequestAndWait(
                    socket,
                    watchId,
                    50,
                    "{\"Keys\":[]}");
            }
        }
        catch
        {
            TryQueueEmergencyDriverReset();
        }
    }

    private static void SendDriverRequestAndWait(
        ClientWebSocket socket,
        string id,
        int type,
        string data)
    {
        using (CancellationTokenSource timeout = new CancellationTokenSource(1800))
        {
            SendDriverRequest(socket, id, type, data, timeout.Token);
            WaitForDriverResponse(socket, id, type, timeout.Token);
        }
    }

    private static void TryQueueEmergencyDriverReset()
    {
        try
        {
            using (ClientWebSocket socket = new ClientWebSocket())
            using (CancellationTokenSource timeout = new CancellationTokenSource(600))
            {
                socket.ConnectAsync(new Uri("ws://127.0.0.1:8083/entry"), timeout.Token)
                    .GetAwaiter().GetResult();
                SendDriverRequest(
                    socket,
                    "iyx-emergency-calibration-" + Guid.NewGuid().ToString("N"),
                    51,
                    "{\"Value\":false}",
                    timeout.Token);
                SendDriverRequest(
                    socket,
                    "iyx-emergency-watch-" + Guid.NewGuid().ToString("N"),
                    50,
                    "{\"Keys\":[]}",
                    timeout.Token);
            }
        }
        catch
        {
        }
    }

    private static void SendDriverRequest(
        ClientWebSocket socket,
        string id,
        int type,
        string data,
        CancellationToken cancellationToken)
    {
        string message = "{\"ID\":\"" + id + "\",\"Type\":" + type + ",\"Data\":" + data + "}";
        byte[] bytes = Encoding.UTF8.GetBytes(message);
        socket.SendAsync(
            new ArraySegment<byte>(bytes),
            WebSocketMessageType.Text,
            true,
            cancellationToken).GetAwaiter().GetResult();
    }

    private static void WaitForDriverResponse(
        ClientWebSocket socket,
        string id,
        int type,
        CancellationToken cancellationToken)
    {
        byte[] buffer = new byte[4096];
        JavaScriptSerializer serializer = new JavaScriptSerializer();
        while (socket.State == WebSocketState.Open)
        {
            using (MemoryStream message = new MemoryStream())
            {
                WebSocketReceiveResult result;
                do
                {
                    result = socket.ReceiveAsync(
                        new ArraySegment<byte>(buffer),
                        cancellationToken).GetAwaiter().GetResult();
                    if (result.MessageType == WebSocketMessageType.Close)
                        throw new InvalidOperationException("驱动服务在确认复位前关闭了连接。");
                    message.Write(buffer, 0, result.Count);
                }
                while (!result.EndOfMessage);

                if (result.MessageType != WebSocketMessageType.Text)
                    continue;

                string text = Encoding.UTF8.GetString(message.ToArray());
                Dictionary<string, object> response;
                try
                {
                    response = serializer.Deserialize<Dictionary<string, object>>(text);
                }
                catch
                {
                    continue;
                }

                if (response == null)
                    continue;

                object responseId;
                if (!response.TryGetValue("ID", out responseId)
                    || !(responseId is string)
                    || !string.Equals((string)responseId, id, StringComparison.Ordinal))
                    continue;

                object responseType;
                long actualType;
                if (!response.TryGetValue("Type", out responseType))
                    throw new InvalidOperationException("驱动服务响应缺少复位类型。");
                if (responseType is int)
                    actualType = (int)responseType;
                else if (responseType is long)
                    actualType = (long)responseType;
                else
                    throw new InvalidOperationException("驱动服务返回了无效的复位响应类型。");
                if (actualType != type)
                    throw new InvalidOperationException("驱动服务返回了不匹配的复位响应类型。");

                object error;
                if (!response.TryGetValue("Error", out error) || !(error is string))
                    throw new InvalidOperationException("驱动服务响应缺少有效的错误状态。");
                if (!string.IsNullOrEmpty((string)error))
                    throw new InvalidOperationException("驱动服务拒绝复位：" + (string)error);

                return;
            }
        }

        throw new InvalidOperationException("驱动服务未确认复位。");
    }

    private static void WaitForLifetime(Process edge, string edgeProfile, Process iyx)
    {
        DateTime edgeEndedAt = DateTime.MinValue;
        while (true)
        {
            if (iyx != null && iyx.HasExited)
                return;

            bool edgeFound = HasEdgeProcess(edgeProfile);
            if (!edgeFound)
            {
                if (edgeEndedAt == DateTime.MinValue)
                    edgeEndedAt = DateTime.UtcNow;
                if ((DateTime.UtcNow - edgeEndedAt).TotalSeconds > 2)
                    return;
            }
            else
            {
                edgeEndedAt = DateTime.MinValue;
            }

            Thread.Sleep(250);
        }
    }

    private static bool HasEdgeProcess(string profile)
    {
        try
        {
            using (ManagementObjectSearcher searcher = new ManagementObjectSearcher(
                "SELECT CommandLine FROM Win32_Process WHERE Name='msedge.exe'"))
            using (ManagementObjectCollection processes = searcher.Get())
            {
                foreach (ManagementObject process in processes)
                {
                    string commandLine = process["CommandLine"] as string;
                    if (!string.IsNullOrEmpty(commandLine)
                        && commandLine.IndexOf(profile, StringComparison.OrdinalIgnoreCase) >= 0)
                        return true;
                }
            }
        }
        catch
        {
        }

        return false;
    }

    private static void StopProcess(Process process)
    {
        try
        {
            if (process != null && !process.HasExited)
            {
                process.Kill();
                process.WaitForExit(3000);
            }
        }
        catch
        {
        }
    }

    private static void TryDeleteDirectory(string path)
    {
        for (int attempt = 0; attempt < 8; attempt++)
        {
            try
            {
                if (!Directory.Exists(path))
                    return;
                Directory.Delete(path, true);
                return;
            }
            catch
            {
                Thread.Sleep(100);
            }
        }
    }

    private sealed class PayloadServer : IDisposable
    {
        private readonly string root;
        private readonly byte[] indexHtml;
        private readonly byte[] sdk;
        private readonly Dictionary<string, byte[]> criticalAssets;
        private readonly HttpListener listener;
        private readonly object keyboardToolLock = new object();
        private Thread acceptThread;
        private Process keyboardTool;
        private volatile bool stopping;
        private int port;

        public PayloadServer(string rootPath)
        {
            root = rootPath;
            indexHtml = BuildIndexHtml();
            sdk = ReadResource(SdkResource);
            criticalAssets = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
            criticalAssets["assets/index-BQvZQSA6.js"] = PatchDriverScript(ReadZipEntry("Data/Local/IYXAST/apps/driver_script/assets/index-BQvZQSA6.js"));
            criticalAssets["assets/root-CDQ7mKD4.js"] = PatchDriverRootScript(ReadZipEntry("Data/Local/IYXAST/apps/driver_script/assets/root-CDQ7mKD4.js"));
            criticalAssets["assets/base-rZDppeNp.js"] = ReadZipEntry("Data/Local/IYXAST/apps/driver_script/assets/base-rZDppeNp.js");
            criticalAssets["assets/sdk-keyboard-naj1DnVU.js"] = ReadZipEntry("Data/Local/IYXAST/apps/driver_script/assets/sdk-keyboard-naj1DnVU.js");
            criticalAssets["assets/index-CVrxRQtY.css"] = ReadZipEntry("Data/Local/IYXAST/apps/driver_script/assets/index-CVrxRQtY.css");
            listener = new HttpListener();
        }

        public int Port
        {
            get { return port; }
        }

        public void Start()
        {
            Random random = new Random();
            for (int attempt = 0; attempt < 20; attempt++)
            {
                port = random.Next(39000, 49000);
                try
                {
                    listener.Prefixes.Clear();
                    listener.Prefixes.Add("http://127.0.0.1:" + port + "/");
                    listener.Start();
                    break;
                }
                catch (HttpListenerException)
                {
                    if (attempt == 19)
                        throw;
                }
            }

            acceptThread = new Thread(AcceptLoop);
            acceptThread.IsBackground = true;
            acceptThread.Start();
        }

        private void AcceptLoop()
        {
            while (!stopping)
            {
                HttpListenerContext context = null;
                try
                {
                    context = listener.GetContext();
                }
                catch
                {
                    if (stopping)
                        return;
                }

                if (context != null)
                    ThreadPool.QueueUserWorkItem(delegate { Handle(context); });
            }
        }

        private void Handle(HttpListenerContext context)
        {
            try
            {
                string path = context.Request.Url.AbsolutePath.TrimStart('/');
                path = Uri.UnescapeDataString(path);

                if (string.Equals(path, "__keyboard-check", StringComparison.OrdinalIgnoreCase))
                {
                    StartKeyboardTool();
                    SendText(context, "keyboard-check started", 200);
                    return;
                }

                if (string.Equals(path, "__driver-cleanup", StringComparison.OrdinalIgnoreCase))
                {
                    TryResetDriverState();
                    SendText(context, "ok", 200);
                    return;
                }

                if (string.Equals(path, "sdk/sdk.js", StringComparison.OrdinalIgnoreCase))
                {
                    SendBytes(context, sdk, "application/javascript", 200);
                    return;
                }

                if (string.Equals(path, "driver_script/index.html", StringComparison.OrdinalIgnoreCase))
                {
                    SendBytes(context, indexHtml, "text/html; charset=utf-8", 200);
                    return;
                }

                const string prefix = "driver_script/";
                if (path.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                {
                    string relativePath = path.Substring(prefix.Length);
                    byte[] cached;
                    if (criticalAssets.TryGetValue(relativePath, out cached))
                    {
                        SendBytes(context, cached, ContentType(relativePath), 200);
                        return;
                    }

                    string entryName = "Data/Local/IYXAST/apps/driver_script/" + relativePath;
                    SendZipEntry(context, entryName);
                    return;
                }

                SendText(context, "Not found", 404);
            }
            catch (Exception ex)
            {
                try
                {
                    SendText(context, ex.Message, 500);
                }
                catch
                {
                }
            }
        }

        private void StartKeyboardTool()
        {
            lock (keyboardToolLock)
            {
                if (keyboardTool != null && !keyboardTool.HasExited)
                    return;

                string tool = ExtractKeyboardTool(root);
                ProcessStartInfo info = new ProcessStartInfo(tool);
                info.WorkingDirectory = Path.GetDirectoryName(tool);
                info.UseShellExecute = true;
                keyboardTool = Process.Start(info);
            }
        }

        private byte[] BuildIndexHtml()
        {
            string text;
            using (ZipArchive archive = OpenArchive())
            {
                ZipArchiveEntry entry = archive.GetEntry("Data/Local/IYXAST/apps/driver_script/index.html");
                if (entry == null)
                    throw new InvalidOperationException("驱动页面不存在。");
                using (Stream input = entry.Open())
                using (StreamReader reader = new StreamReader(input, Encoding.UTF8))
                {
                    text = reader.ReadToEnd();
                }
            }

            text = text.Replace("https://user.joyway.net/sdk/sdk.js?20201202", "/sdk/sdk.js");
            const string bridge = @"
<style id='toolbox-theme'>
:root {
  --color-brand-1: #c8f43d !important;
  --color-brand-1-rgb: 200, 244, 61 !important;
  --color-brand-2: #2864ff !important;
  --color-brand-2-rgb: 40, 100, 255 !important;
  --color-dark: #10110f !important;
  --color-text-hint: #4c5048 !important;
  --brand-color: #10110f !important;
  --brand-color-pure: #2864ff !important;
  --brand-color-rgb: 40, 100, 255 !important;
  --brand-color-pure-rgb: 40, 100, 255 !important;
  --brand-accent-color: #c8f43d !important;
  --brand-accent-rgb: 200, 244, 61 !important;
  --bg-main: #f3f4f0 !important;
  --bg-card: #ffffff !important;
  --surface-text-primary: #10110f !important;
  --surface-text-secondary: #4c5048 !important;
  --shell-text-primary: #10110f !important;
  --shell-text-secondary: #4c5048 !important;
  --shell-divider: #d3d6cf !important;
  --border-color: #d3d6cf !important;
  --radius-s: 4px !important;
  --radius-m: 6px !important;
  --radius-l: 8px !important;
  --shadow-sm: 0 2px 0 rgba(16,17,15,.1) !important;
  --shadow-md: 0 5px 0 rgba(16,17,15,.12) !important;
  --shadow-lg: 0 8px 0 rgba(16,17,15,.14) !important;
  --shadow-panel: 0 5px 0 rgba(16,17,15,.1) !important;
  --shadow-soft: 0 2px 0 rgba(16,17,15,.08) !important;
  --header-shell-bg: #ffffff !important;
  --header-surface: #ffffff !important;
  --sidebar-surface: #ffffff !important;
  --sidebar-surface-hover: #eef0eb !important;
  --sidebar-item-active: #c8f43d !important;
  --settings-shell-bg: #f3f4f0 !important;
  --settings-sidebar-bg: #ffffff !important;
  --settings-sidebar-item-hover-bg: #eef0eb !important;
  --settings-sidebar-item-active-bg: #c8f43d !important;
  --settings-right-bg: #ffffff !important;
  --settings-card-bg: #ffffff !important;
  --settings-card-border: #d3d6cf !important;
  --btn-base-bg: #ffffff !important;
  --btn-base-hover-bg: #eef0eb !important;
  --btn-base-text: #10110f !important;
  --btn-base-hover-text: #10110f !important;
  --btn-base-border-color: #d3d6cf !important;
  --action-btn-primary-bg: #10110f !important;
  --action-btn-primary-bg-solid: #10110f !important;
  --action-btn-primary-shadow: 0 4px 0 rgba(40,100,255,.24) !important;
  --action-btn-primary-shadow-hover: 0 6px 0 rgba(40,100,255,.2) !important;
  --vk-board-bg: #ffffff !important;
  --vk-board-border: 1px solid #d3d6cf !important;
  --vk-board-shadow: 0 5px 0 rgba(16,17,15,.1) !important;
  --vk-key-bg: #ffffff !important;
  --vk-key-hover-bg: #c8f43d !important;
  --vk-key-shadow: 0 2px 0 rgba(16,17,15,.12) !important;
  --vk-key-hover-shadow: 0 4px 0 rgba(16,17,15,.14) !important;
  --vk-key-radius: 5px !important;
  --vk-accent-color: #2864ff !important;
  --vk-accent-gradient: #2864ff !important;
}
html, body {
  color: #10110f !important;
  font-family: 'Microsoft YaHei UI', 'Segoe UI', sans-serif !important;
  letter-spacing: 0 !important;
  background-color: #f3f4f0 !important;
  background-image:
    linear-gradient(#e1e3de 1px, transparent 1px),
    linear-gradient(90deg, #e1e3de 1px, transparent 1px) !important;
  background-size: 42px 42px !important;
}
*, *::before, *::after { letter-spacing: 0 !important; }
::-webkit-scrollbar { width: 8px !important; height: 8px !important; }
::-webkit-scrollbar-track { background: #eef0eb !important; }
::-webkit-scrollbar-thumb { background: #10110f !important; border-radius: 4px !important; }
.header-container {
  color: #10110f !important;
  background: #ffffff !important;
  border-bottom: 3px solid #2864ff !important;
  box-shadow: none !important;
}
.header-container button, .header-container select,
.header-container .item, .header-container .language_main {
  color: #10110f !important;
  border-color: #d3d6cf !important;
}
.main_container, .main_container > .container,
.background-main, .new_background-main {
  background-color: transparent !important;
  background-image: none !important;
  box-shadow: none !important;
}
button, input, select, textarea, [role=button] {
  border-radius: 6px !important;
  box-shadow: none !important;
  font-family: 'Microsoft YaHei UI', 'Segoe UI', sans-serif !important;
  transition: transform .28s cubic-bezier(.2,.8,.2,1),
              background-color .28s ease, border-color .28s ease !important;
}
button:hover, [role=button]:hover { transform: translateY(-2px); }
button:active, [role=button]:active { transform: translateY(1px); }
input, select, textarea {
  color: #10110f !important;
  background: #fff !important;
  border: 1px solid #d3d6cf !important;
}
.set_b_box .set_box, .feedback_b_box .feedback_box,
.dialog-view .dialog-message, .set-action-overlay .content,
.service_update .service_update_box, .firmware-upgrade,
.app_update_notice_container {
  background: #fff !important;
  border: 1px solid #10110f !important;
  border-radius: 8px !important;
  box-shadow: 8px 8px 0 rgba(16,17,15,.14) !important;
  backdrop-filter: none !important;
}
.selected, .active, [aria-selected=true] {
  border-color: #2864ff !important;
}
.toolbox-system-mark {
  position: fixed;
  left: 16px;
  bottom: 16px;
  z-index: 2147483646;
  height: 38px;
  padding: 0 14px;
  display: flex;
  align-items: center;
  gap: 9px;
  color: #10110f;
  background: #f3f4f0;
  border: 1px solid #10110f;
  border-radius: 6px;
  font: 700 11px/1 Bahnschrift, 'Segoe UI', sans-serif;
  box-shadow: 3px 3px 0 rgba(16,17,15,.14);
}
.toolbox-system-mark::before {
  content: '';
  width: 5px;
  height: 16px;
  background: #c8f43d;
}
.toolbox-keyboard-launcher {
  position: fixed !important;
  right: 16px !important;
  bottom: 16px !important;
  z-index: 2147483647 !important;
  width: 52px !important;
  height: 52px !important;
  padding: 0 !important;
  display: grid !important;
  place-items: center !important;
  color: #10110f !important;
  background: #c8f43d !important;
  border: 1px solid #10110f !important;
  border-radius: 6px !important;
  box-shadow: 4px 4px 0 rgba(16,17,15,.2) !important;
}
.toolbox-keyboard-launcher:hover {
  background: #d6ff54 !important;
  box-shadow: 6px 6px 0 rgba(16,17,15,.16) !important;
}
.toolbox-keyboard-launcher svg { width: 24px; height: 24px; }
html body .device_not_connect,
html body .device_sleep {
  color: #10110f !important;
  background: rgba(16,17,15,.58) !important;
  backdrop-filter: none !important;
}
html body .device_not_connect .message_top_abnormal,
html body .device_sleep .message_top_abnormal {
  background: #fff !important;
  border: 1px solid #10110f !important;
  border-radius: 8px !important;
  box-shadow: 10px 10px 0 rgba(16,17,15,.18) !important;
  backdrop-filter: none !important;
}
html body .device_not_connect .exit_sleep,
html body .device_sleep .exit_sleep {
  color: #fff !important;
  background: #10110f !important;
  border: 1px solid #10110f !important;
  border-radius: 6px !important;
  box-shadow: 4px 4px 0 rgba(40,100,255,.28) !important;
  backdrop-filter: none !important;
}
html body .device_not_connect .message_action,
html body .device_not_connect .message_action_kb,
html body .device_not_connect .kb_message,
html body .device_sleep .message_action,
html body .device_sleep .message_action_kb,
html body .device_sleep .kb_message {
  color: #4c5048 !important;
  text-shadow: none !important;
}
</style>
<script>
(function () {
  document.title = 'IYX / DRIVER CONTROL';
  window.electronAPI = window.electronAPI || {
    close: function () {
      var closed = false;
      var finish = function () {
        if (closed) return;
        closed = true;
        window.close();
      };
      try {
        fetch('/__driver-cleanup', { method: 'POST', cache: 'no-store' }).then(finish, finish);
        setTimeout(finish, 2600);
      } catch (error) {
        finish();
      }
    },
    min: function () {},
    openDev: function () {},
    openExternal: function (url) { window.open(url, '_blank'); },
    wnd: function () { return Promise.resolve(null); },
    drag: function () {},
    on: function () {},
    SetWindowPos: function () {},
    setPositionByScreenAndScale: function () {},
    showMenu: function () {}
  };
  window.electron = { ipcRenderer: { send: function () {} } };
  window.require = window.require || function () { return { ipcRenderer: { send: function () {} } }; };
  window.addEventListener('DOMContentLoaded', function () {
    var enforceToolboxTheme = function () {
      document.title = 'IYX / DRIVER CONTROL';
      document.querySelectorAll('.exit_sleep').forEach(function (element) {
        element.style.setProperty('color', '#ffffff', 'important');
        element.style.setProperty('background', '#10110f', 'important');
        element.style.setProperty('border', '1px solid #10110f', 'important');
        element.style.setProperty('border-radius', '6px', 'important');
        element.style.setProperty('box-shadow', '4px 4px 0 rgba(40,100,255,.28)', 'important');
      });
      document.querySelectorAll('.message_top_abnormal').forEach(function (element) {
        element.style.setProperty('background', '#ffffff', 'important');
        element.style.setProperty('border', '1px solid #10110f', 'important');
        element.style.setProperty('border-radius', '8px', 'important');
        element.style.setProperty('box-shadow', '10px 10px 0 rgba(16,17,15,.18)', 'important');
        element.style.setProperty('backdrop-filter', 'none', 'important');
      });
      document.querySelectorAll('button, [role=button], a, div').forEach(function (element) {
        var label = (element.textContent || '').trim();
        if (label !== '\u5173\u95ed' && label !== 'Exit Sleep Mode') return;
        element.style.setProperty('color', '#ffffff', 'important');
        element.style.setProperty('background', '#10110f', 'important');
        element.style.setProperty('border', '1px solid #10110f', 'important');
        element.style.setProperty('border-radius', '6px', 'important');
        element.style.setProperty('box-shadow', '4px 4px 0 rgba(40,100,255,.28)', 'important');
      });
    };
    document.documentElement.setAttribute('software-theme-preset', 'daylight');
    var favicon = document.getElementById('favicon');
    if (favicon) {
      favicon.href = `data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'%3E%3Crect width='24' height='24' rx='4' fill='%2310110f'/%3E%3Cpath d='m12 14 4-4M3.34 19a10 10 0 1 1 17.32 0' fill='none' stroke='%23c8f43d' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'/%3E%3C/svg%3E`;
    }
    var mark = document.createElement('div');
    mark.className = 'toolbox-system-mark';
    mark.textContent = 'IYX / DRIVER 03';
    document.body.appendChild(mark);
    var button = document.createElement('button');
    button.type = 'button';
    button.title = '\u952e\u76d8\u68c0\u67e5';
    button.setAttribute('aria-label', '\u952e\u76d8\u68c0\u67e5');
    button.className = 'toolbox-keyboard-launcher';
    button.innerHTML = `<svg viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round' aria-hidden='true'><path d='M10 8h.01'/><path d='M12 12h.01'/><path d='M14 8h.01'/><path d='M16 12h.01'/><path d='M18 8h.01'/><path d='M6 8h.01'/><path d='M7 16h10'/><path d='M8 12h.01'/><rect width='20' height='16' x='2' y='4' rx='2'/></svg>`;
    button.addEventListener('click', function () { fetch('/__keyboard-check').catch(function () {}); });
    document.body.appendChild(button);
    enforceToolboxTheme();
    new MutationObserver(enforceToolboxTheme).observe(document.body, {
      childList: true,
      subtree: true
    });
  });
}());
</script>
";

            int head = text.IndexOf("</head>", StringComparison.OrdinalIgnoreCase);
            if (head < 0)
                return Encoding.UTF8.GetBytes(bridge + text);
            text = text.Insert(head, bridge);
            return Encoding.UTF8.GetBytes(text);
        }

        private void SendZipEntry(HttpListenerContext context, string entryName)
        {
            using (ZipArchive archive = OpenArchive())
            {
                ZipArchiveEntry entry = archive.GetEntry(entryName);
                if (entry == null)
                {
                    SendText(context, "Not found", 404);
                    return;
                }

                context.Response.StatusCode = 200;
                context.Response.ContentType = ContentType(entryName);
                context.Response.ContentLength64 = entry.Length;
                if (!string.Equals(context.Request.HttpMethod, "HEAD", StringComparison.OrdinalIgnoreCase))
                {
                    using (Stream input = entry.Open())
                    {
                        input.CopyTo(context.Response.OutputStream, 1024 * 1024);
                    }
                }
                context.Response.Close();
            }
        }

        private static string ContentType(string name)
        {
            string ext = Path.GetExtension(name).ToLowerInvariant();
            switch (ext)
            {
                case ".html": return "text/html; charset=utf-8";
                case ".js": return "application/javascript";
                case ".css": return "text/css";
                case ".json": return "application/json";
                case ".svg": return "image/svg+xml";
                case ".png": return "image/png";
                case ".jpg":
                case ".jpeg": return "image/jpeg";
                case ".webp": return "image/webp";
                case ".ico": return "image/x-icon";
                case ".woff": return "font/woff";
                case ".woff2": return "font/woff2";
                default: return "application/octet-stream";
            }
        }

        private static byte[] ReadResource(string name)
        {
            using (Stream input = Assembly.GetExecutingAssembly().GetManifestResourceStream(name))
            using (MemoryStream output = new MemoryStream())
            {
                if (input == null)
                    throw new InvalidOperationException("内置资源不存在：" + name);
                input.CopyTo(output, 1024 * 1024);
                return output.ToArray();
            }
        }

        private static byte[] ReadZipEntry(string entryName)
        {
            using (ZipArchive archive = OpenArchive())
            {
                ZipArchiveEntry entry = archive.GetEntry(entryName);
                if (entry == null)
                    throw new InvalidOperationException("驱动资源不存在：" + entryName);
                using (Stream input = entry.Open())
                using (MemoryStream output = new MemoryStream())
                {
                    input.CopyTo(output, 1024 * 1024);
                    return output.ToArray();
                }
            }
        }

        private static byte[] PatchDriverScript(byte[] bytes)
        {
            string text = Encoding.UTF8.GetString(bytes);
            const string automaticCalibration = "initializeCalibrationState(){this.loadCalibrationStatus(),!this.state.initCalibrationStatus&&this.showCalibrationPageByVendor()}";
            const string skipAutomaticCalibration = "initializeCalibrationState(){this.loadCalibrationStatus()}";
            text = ReplaceExactlyOnce(
                text,
                automaticCalibration,
                skipAutomaticCalibration,
                "skip automatic calibration");

            const string delayedCurrentDevice = "const s=await this.askType(S.MTUsingDevice);if(s){if(await this.askDeviceKeys(),";
            const string earlyCurrentDevice = "const s=await this.askType(S.MTUsingDevice);if(s){if(I.instance.setCurrentDevice(s.ID),await this.askDeviceKeys(),";
            text = ReplaceExactlyOnce(
                text,
                delayedCurrentDevice,
                earlyCurrentDevice,
                "initialize current device before key definitions");

            const string duplicateCurrentDevice = "if(I.instance.setCurrentDevice(s.ID),U.checkFirmwareUpdate(),";
            text = ReplaceExactlyOnce(
                text,
                duplicateCurrentDevice,
                "if(U.checkFirmwareUpdate(),",
                "remove duplicate current device update");
            return Encoding.UTF8.GetBytes(text);
        }

        private static byte[] PatchDriverRootScript(byte[] bytes)
        {
            string text = Encoding.UTF8.GetString(bytes);
            const string calibrationUnmount = "Kn=A({__name:\"calibration\",setup(v){return be(()=>{U.pauseDynamicImageForCalibration()}),Ge(()=>{U.resumeDynamicImageAfterCalibration()}),";
            const string patchedCalibrationUnmount = "Kn=A({__name:\"calibration\",setup(v){return be(()=>{U.pauseDynamicImageForCalibration()}),Ge(()=>{Promise.resolve(U.calibrationAllKeys(!1)).catch(()=>{}).finally(()=>Promise.resolve(U.endAllKeysWatchTrave()).catch(()=>{})),U.resumeDynamicImageAfterCalibration()}),";
            text = ReplaceExactlyOnce(
                text,
                calibrationUnmount,
                patchedCalibrationUnmount,
                "clean up calibration when its view unmounts");

            const string rootCleanup = "function nu(){Bt.clear(),U.resetCalibrationStatus(),z.fnCheck.value=!1,Vs.state.rateValue=0,z.changeActionType(T.none),De.clear(),gi.isShowLeft=!1,Ie.state.isShowSetKeyGroup=!1,Mt.value=!1}";
            const string patchedRootCleanup = "const iyxFastCleanupCalibration=()=>{try{He.instance.getSocketService.send({ID:\"iyx-page-calibration-\"+Date.now(),Type:st.MTMagnetCalibratin,Data:{Value:!1}})}catch{}try{He.instance.getSocketService.send({ID:\"iyx-page-watch-\"+Date.now(),Type:st.MTMagnetSetWatch,Data:{Keys:[]}})}catch{}U.clearDynamicImagePauseForCalibration()};window.addEventListener(\"pagehide\",iyxFastCleanupCalibration,!0),window.addEventListener(\"beforeunload\",iyxFastCleanupCalibration,!0);function nu(){Bt.clear(),U.resetCalibrationStatus(),z.fnCheck.value=!1,Vs.state.rateValue=0,z.changeActionType(T.none),De.clear(),gi.isShowLeft=!1,Ie.state.isShowSetKeyGroup=!1,Mt.value=!1}";
            text = ReplaceExactlyOnce(
                text,
                rootCleanup,
                patchedRootCleanup,
                "clean up calibration when the driver page closes");
            return Encoding.UTF8.GetBytes(text);
        }

        private static string ReplaceExactlyOnce(
            string text,
            string oldValue,
            string newValue,
            string patchName)
        {
            int index = text.IndexOf(oldValue, StringComparison.Ordinal);
            if (index < 0)
                throw new InvalidOperationException("驱动资源补丁不匹配：" + patchName);
            if (text.IndexOf(oldValue, index + oldValue.Length, StringComparison.Ordinal) >= 0)
                throw new InvalidOperationException("驱动资源补丁命中多次：" + patchName);
            return text.Substring(0, index)
                + newValue
                + text.Substring(index + oldValue.Length);
        }

        private static ZipArchive OpenArchive()
        {
            Stream payload = Assembly.GetExecutingAssembly().GetManifestResourceStream(PayloadResource);
            if (payload == null)
                throw new InvalidOperationException("内置程序文件不存在。");
            return new ZipArchive(payload, ZipArchiveMode.Read, false);
        }

        private static void SendBytes(HttpListenerContext context, byte[] bytes, string contentType, int status)
        {
            context.Response.StatusCode = status;
            context.Response.ContentType = contentType;
            context.Response.ContentLength64 = bytes.Length;
            if (!string.Equals(context.Request.HttpMethod, "HEAD", StringComparison.OrdinalIgnoreCase))
                context.Response.OutputStream.Write(bytes, 0, bytes.Length);
            context.Response.Close();
        }

        private static void SendText(HttpListenerContext context, string text, int status)
        {
            SendBytes(context, Encoding.UTF8.GetBytes(text), "text/plain; charset=utf-8", status);
        }

        public void Dispose()
        {
            stopping = true;
            lock (keyboardToolLock)
            {
                StopProcess(keyboardTool);
                keyboardTool = null;
            }
            try { listener.Stop(); } catch { }
            try { listener.Close(); } catch { }
            if (acceptThread != null && acceptThread.IsAlive)
                acceptThread.Join(500);
        }
    }

    private sealed class BrowserBridge : IDisposable
    {
        private readonly ClientWebSocket socket = new ClientWebSocket();
        private readonly CancellationTokenSource cancellation = new CancellationTokenSource();
        private Thread receiveThread;
        private int launchSent;

        public void Connect()
        {
            socket.ConnectAsync(new Uri("ws://127.0.0.1:7678"), cancellation.Token)
                .GetAwaiter().GetResult();
            Send("{\"ID\":\"fast-register\",\"Type\":1,\"Data\":\"browser\",\"Err\":\"\"}");

            receiveThread = new Thread(ReceiveLoop);
            receiveThread.IsBackground = true;
            receiveThread.Start();
        }

        private void ReceiveLoop()
        {
            byte[] buffer = new byte[64 * 1024];
            while (!cancellation.IsCancellationRequested && socket.State == WebSocketState.Open)
            {
                try
                {
                    using (MemoryStream message = new MemoryStream())
                    {
                        WebSocketReceiveResult result;
                        do
                        {
                            result = socket.ReceiveAsync(
                                new ArraySegment<byte>(buffer), cancellation.Token)
                                .GetAwaiter().GetResult();
                            if (result.MessageType == WebSocketMessageType.Close)
                                return;
                            message.Write(buffer, 0, result.Count);
                        }
                        while (!result.EndOfMessage);

                        string text = Encoding.UTF8.GetString(message.ToArray());
                        if (text.IndexOf("\"Data\":\"welcome\"", StringComparison.Ordinal) >= 0
                            && Interlocked.Exchange(ref launchSent, 1) == 0)
                        {
                            Send("{\"ID\":\"fast-launch\",\"Type\":4,\"Data\":\"driver_script\",\"Err\":\"\"}");
                        }
                    }
                }
                catch
                {
                    return;
                }
            }
        }

        private void Send(string text)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(text);
            socket.SendAsync(
                new ArraySegment<byte>(bytes),
                WebSocketMessageType.Text,
                true,
                cancellation.Token).GetAwaiter().GetResult();
        }

        public void Dispose()
        {
            try { cancellation.Cancel(); } catch { }
            try { socket.Abort(); } catch { }
            try { socket.Dispose(); } catch { }
            if (receiveThread != null && receiveThread.IsAlive)
                receiveThread.Join(300);
            cancellation.Dispose();
        }
    }
}
