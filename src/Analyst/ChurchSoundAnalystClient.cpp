#include "ChurchSoundAnalystClient.h"

namespace churchstream
{
class ChurchSoundAnalystClient::Job final : public juce::ThreadPoolJob
{
public:
    Job(juce::String tokenIn, juce::File fileIn, Completion callbackIn)
        : ThreadPoolJob("Church Sound Analyst"), token(std::move(tokenIn)), file(std::move(fileIn)), callback(std::move(callbackIn)) {}

    JobStatus runJob() override
    {
        if (!file.existsAsFile()) return finish(false, "Archivo no encontrado");
        auto object = juce::var(new juce::DynamicObject());
        object.getDynamicObject()->setProperty("path", file.getFullPathName());
        const auto url = juce::URL("http://127.0.0.1:8765/analyze-path");
        const auto headers = "Content-Type: application/json\r\nX-API-Token: " + token + "\r\n";
        auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inPostData)
            .withHttpRequestCmd("POST")
            .withExtraHeaders(headers)
            .withConnectionTimeoutMs(2500)
            .withNumRedirectsToFollow(0);
        auto stream = url.withPOSTData(juce::JSON::toString(object)).createInputStream(options);
        if (stream == nullptr) return finish(false, "Church Sound Analyst no está disponible");
        const auto response = stream->readEntireStreamAsString();
        if (response.isEmpty()) return finish(false, "El analizador devolvió una respuesta vacía");
        return finish(true, response);
    }

private:
    JobStatus finish(bool ok, juce::String message)
    {
        juce::MessageManager::callAsync([callback = std::move(callback), ok, message = std::move(message)]() mutable
        {
            if (callback) callback(ok, std::move(message));
        });
        return jobHasFinished;
    }

    juce::String token;
    juce::File file;
    Completion callback;
};

ChurchSoundAnalystClient::ChurchSoundAnalystClient(juce::String tokenIn) : token(std::move(tokenIn)) {}
ChurchSoundAnalystClient::~ChurchSoundAnalystClient() { pool.removeAllJobs(true, 3000); }

void ChurchSoundAnalystClient::analyzeFile(const juce::File& file, Completion completion)
{
    pool.addJob(new Job(token, file, std::move(completion)), true);
}
} // namespace churchstream
