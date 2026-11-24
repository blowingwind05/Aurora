using System.Threading.Tasks;

namespace Aurora;

public sealed partial class AuroraWindow
{
    private QtShellMoreState qtMoreState = new(AuroraMore.VersionLabel, AuroraMore.FeedbackAddress);

    private async Task HandleQtMoreRequest(string action)
    {
        var bridge = qtShell;
        if (action == "checkUpdates")
        {
            if (qtMoreState.Checking) return;
            qtMoreState = qtMoreState with { Checking = true };
            PublishQtShellState();

            var result = await AuroraMore.CheckLatestRelease();
            qtMoreState = qtMoreState with
            {
                Checking = false,
                UpdateCompleted = qtMoreState.UpdateCompleted + 1
            };
            if (qtShell == bridge && !closed) PublishQtMoreNotice(result);
        }
        else if (await AuroraMore.OpenProjectLink(this, action) is { } error && qtShell == bridge && !closed)
        {
            PublishQtMoreNotice(error);
        }
    }

    private void PublishQtMoreNotice(string text)
    {
        qtMoreState = qtMoreState with { Notice = text, NoticeId = qtMoreState.NoticeId + 1 };
        PublishQtShellState();
    }
}
