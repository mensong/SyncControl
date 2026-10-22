using System.IO.Pipes;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace SyncControl.GUI.Services;

public sealed record IpcResponse(bool Success, JsonObject? Response, string Error = "")
{
    public static readonly IpcResponse Failed = new(false, null, "IPC unavailable");
}

public sealed class IpcClient : IDisposable
{
    private const string PipeName = "SyncControl";
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    private readonly SemaphoreSlim _requestLock = new(1, 1);

    public event Action<JsonObject>? StatusReceived;
    public event Action<bool>? ConnectionChanged;

    public void Dispose()
    {
        _requestLock.Dispose();
    }

    public async Task<IpcResponse> SendAsync(JsonObject command, CancellationToken cancellationToken = default)
    {
        await _requestLock.WaitAsync(cancellationToken);
        try
        {
            return await SendOnceAsync(command, cancellationToken);
        }
        finally
        {
            _requestLock.Release();
        }
    }

    // A cold-started Core may still be creating its first pipe instance when
    // the GUI sends start_master. Probe the pipe before sending that command.
    public async Task<bool> WaitForReadyAsync(int timeoutMs = 5000,
                                              CancellationToken cancellationToken = default)
    {
        var deadline = Environment.TickCount64 + timeoutMs;
        while (Environment.TickCount64 < deadline)
        {
            // Reuse the serialized request path. A raw probe can race with the
            // status poll on the Core's single active pipe instance.
            try
            {
                using var attemptSource = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
                attemptSource.CancelAfter(1000);
                var response = await SendAsync(new JsonObject
                {
                    ["type"] = "get_status",
                }, attemptSource.Token);
                if (response.Success)
                {
                    return true;
                }
            }
            catch (Exception error) when (!cancellationToken.IsCancellationRequested &&
                error is OperationCanceledException or TimeoutException or IOException
                    or UnauthorizedAccessException)
            {
            }

            try
            {
                await Task.Delay(100, cancellationToken);
            }
            catch (OperationCanceledException)
            {
                break;
            }
        }

        return false;
    }

    public async Task PollForeverAsync(CancellationToken cancellationToken)
    {
        var lastConnected = false;
        while (!cancellationToken.IsCancellationRequested)
        {
            var response = await SendAsync(new JsonObject { ["type"] = "get_status" }, cancellationToken);
            if (response.Success != lastConnected)
            {
                lastConnected = response.Success;
                ConnectionChanged?.Invoke(response.Success);
            }

            if (response.Response is { } status && status["type"]?.GetValue<string>() == "status")
            {
                StatusReceived?.Invoke(status);
            }

            try
            {
                await Task.Delay(1000, cancellationToken);
            }
            catch (OperationCanceledException)
            {
                break;
            }
        }
    }

    private static async Task<IpcResponse> SendOnceAsync(JsonObject command, CancellationToken cancellationToken)
    {
        await using NamedPipeClientStream? pipe = new(".", PipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
        try
        {
            // ConnectAsync's short-timeout path has proved unreliable on some
            // Windows hosts ("The semaphore timeout period has expired").
            // The synchronous API is stable, so run it on the thread pool.
            var connectTask = Task.Run(() =>
            {
                pipe.Connect(900);
                return true;
            }, CancellationToken.None);
            var timeoutTask = Task.Delay(1000, cancellationToken);
            var completed = await Task.WhenAny(connectTask, timeoutTask);
            if (completed != connectTask)
            {
                return IpcResponse.Failed;
            }
            await connectTask;

            pipe.ReadMode = PipeTransmissionMode.Message;
            var request = JsonSerializer.Serialize(command, JsonOptions);
            var bytes = Encoding.UTF8.GetBytes(request);
            await pipe.WriteAsync(bytes, cancellationToken);
            await pipe.FlushAsync(cancellationToken);

            var responseText = await ReadMessageAsync(pipe, cancellationToken);
            if (responseText is null)
            {
                return IpcResponse.Failed;
            }

            var response = JsonNode.Parse(responseText) as JsonObject;
            return response is null
                ? new IpcResponse(false, null, "Invalid IPC response")
                : new IpcResponse(true, response);
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (Exception)
        {
            return IpcResponse.Failed;
        }
    }

    private static async Task<string?> ReadMessageAsync(PipeStream pipe, CancellationToken cancellationToken)
    {
        using var stream = new MemoryStream();
        var buffer = new byte[4096];
        while (true)
        {
            var read = await pipe.ReadAsync(buffer, cancellationToken);
            if (read == 0)
            {
                break;
            }
            await stream.WriteAsync(buffer.AsMemory(0, read), cancellationToken);
            if (pipe.IsMessageComplete)
            {
                break;
            }
        }

        return stream.Length == 0 ? null : Encoding.UTF8.GetString(stream.ToArray());
    }
}
