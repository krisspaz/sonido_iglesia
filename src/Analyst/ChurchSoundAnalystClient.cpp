#include "ChurchSoundAnalystClient.h"

namespace churchstream
{
class ChurchSoundAnalystClient::Job final : public juce::ThreadPoolJob
{
public:
    Job(juce::String tokenIn, juce::String endpointIn, juce::var payloadIn, Completion callbackIn)
        : ThreadPoolJob("Church Sound Analyst"), token(std::move(tokenIn)), endpoint(std::move(endpointIn)),
          payload(std::move(payloadIn)), callback(std::move(callbackIn)) {}

    JobStatus runJob() override
    {
        const auto url = juce::URL("http://127.0.0.1:8765" + endpoint);
        const auto headers = "Content-Type: application/json\r\nX-API-Token: " + token + "\r\n";
        auto options = juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inPostData)
            .withHttpRequestCmd("POST")
            .withExtraHeaders(headers)
            .withConnectionTimeoutMs(2500)
            .withNumRedirectsToFollow(0);
        auto stream = url.withPOSTData(juce::JSON::toString(payload)).createInputStream(options);
        if (stream == nullptr) return finish(false, "Church Sound Analyst no está disponible");
        const auto response = stream->readEntireStreamAsString();
        if (response.isEmpty()) return finish(false, "El analizador devolvió una respuesta vacía");
        const auto parsed = juce::JSON::parse(response);
        if (auto* error = parsed.getDynamicObject(); error != nullptr && error->hasProperty("detail"))
            return finish(false, error->getProperty("detail").toString());
        return finish(true, response);
    }

private:
    JobStatus finish(bool ok, juce::String responseText)
    {
        auto completionForUi = std::move(callback);
        juce::MessageManager::callAsync([done = std::move(completionForUi), ok, reply = std::move(responseText)]() mutable
        {
            if (done) done(ok, std::move(reply));
        });
        return jobHasFinished;
    }

    juce::String token;
    juce::String endpoint;
    juce::var payload;
    Completion callback;
};

ChurchSoundAnalystClient::ChurchSoundAnalystClient(juce::String tokenIn) : token(std::move(tokenIn))
{
    startBundledService();
}

ChurchSoundAnalystClient::~ChurchSoundAnalystClient()
{
    pool.removeAllJobs(true, 3000);
    if (bundledService.isRunning()) bundledService.kill();
}

void ChurchSoundAnalystClient::startBundledService()
{
#if JUCE_WINDOWS
    const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    const auto service = executable.getParentDirectory().getChildFile("ChurchSoundAnalyst")
                                   .getChildFile("ChurchSoundAnalyst.exe");
    const auto tokenFile = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                               .getChildFile("ChurchStreamProcessor").getChildFile("church-sound-analyst.token");
    const auto database = tokenFile.getParentDirectory().getChildFile("church-sound-learning.sqlite3");
    if (!service.existsAsFile() || !tokenFile.existsAsFile()) return;
    const auto quote = [](const juce::File& file) { return "\"" + file.getFullPathName() + "\""; };
    bundledService.start(quote(service) + " --token-file " + quote(tokenFile) + " --database " + quote(database));
#endif
}

void ChurchSoundAnalystClient::analyzeFile(const juce::File& file, Completion completion)
{
    if (!file.existsAsFile())
    {
        juce::MessageManager::callAsync([callback = std::move(completion)]() mutable
        {
            if (callback) callback(false, "Archivo no encontrado");
        });
        return;
    }
    auto payload = juce::var(new juce::DynamicObject());
    payload.getDynamicObject()->setProperty("path", file.getFullPathName());
    request("/analyze-full", std::move(payload), std::move(completion));
}

void ChurchSoundAnalystClient::request(const juce::String& endpoint, juce::var payload, Completion completion)
{
    if (!endpoint.startsWithChar('/'))
    {
        if (completion) completion(false, "Endpoint local inválido");
        return;
    }
    pool.addJob(new Job(token, endpoint, std::move(payload), std::move(completion)), true);
}
} // namespace churchstream
