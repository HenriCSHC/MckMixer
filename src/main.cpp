#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <pwd.h>

// Web
#include "App.h"
#include <filesystem>
#include <sstream>
#include <fstream>
#include <string>
#include <string.h>

// Types
#include "nlohmann/json.hpp"
#include "MckTypes.h"

// Dsp Modules
#include "MckMixer.h"

namespace fs = std::filesystem;
using namespace nlohmann;

struct PerSocketData
{
    std::string id;
    float rand;
};

mck::Mixer m_mixer;

static void SignalHandler(int sig)
{
    fprintf(stdout, "Signal %d received, exiting...\n", sig);

    m_mixer.Close();
    exit(0);
}

int main(int argc, char **argv)
{
    struct passwd *pw = getpwuid(getuid());
    fs::path configPath(pw->pw_dir);
    configPath.append(".mck").append("mixer");
    if (fs::exists(configPath) == false)
    {
        fs::create_directories(configPath);
    }
    configPath.append("config.json");

    if (m_mixer.Init(configPath.string()) == false)
    {
        return EXIT_FAILURE;
    }

    signal(SIGQUIT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    signal(SIGHUP, SignalHandler);
    signal(SIGINT, SignalHandler);

    //sleep(-1);

    /* Keep in mind that uWS::SSLApp({options}) is the same as uWS::App() when compiled without SSL support.
     * You may swap to using uWS:App() if you don't need SSL */
    uWS::App().get("/*", [](uWS::HttpResponse<false> *res, uWS::HttpRequest *req) {
                  std::cout << "Requested: " << req->getUrl() << " - " << req->getQuery() << std::endl;
                  res->writeStatus(uWS::HTTP_200_OK);
                  fs::path p("www");
                  std::string url(req->getUrl());
                  fs::path u(url);
                  u = u.relative_path();
                  bool sendFile = true;
                  if (url == "/" || url == "")
                  {
                      p.append("index.html");
                  }
                  else
                  {
                      p.append(u.string());
                  }
                  if (fs::exists(p) == false)
                  {
                      if (p.extension() == "html")
                      {
                          p = fs::path("www").append("404.html");
                      }
                      else
                      {
                          res->writeStatus("404");
                          sendFile = false;
                      }
                  }
                  if (sendFile)
                  {
                      std::fstream fstr = std::fstream(p);
                      std::string str(std::istreambuf_iterator<char>(fstr), {});
                      fstr.close();
                      res->end(str);
                  }
                  else
                  {
                      res->end();
                  }
              })
        .ws<PerSocketData>("/*", {/* Settings 
                                  .compression = uWS::SHARED_COMPRESSOR,
                                  .maxPayloadLength = 16 * 1024,
                                  .idleTimeout = 10,
                                  .maxBackpressure = 1 * 1024 * 1204,
                                  */
                                  /* Handlers */
                                  .open = [&](auto *ws, auto *req) {
            PerSocketData *userData = (PerSocketData *)ws->getUserData();
            userData->id = "hoi";
            mck::Message msg("config", "partial");
            mck::Config config;
            m_mixer.GetConfig(config);
            json j = config;
            msg.data = j.dump();
            json jOut = msg;
            ws->send(jOut.dump(), uWS::OpCode::TEXT);
            /* Open event here, you may access ws->getUserData() which points to a PerSocketData struct */ },
                                  .message = [&](auto *ws, std::string_view message, uWS::OpCode opCode) {
            PerSocketData *userData = (PerSocketData *)ws->getUserData();
            std::cout << "MSG from " << userData->id << ": " << message << std::endl;
            mck::Message msg;
            try
            {
                json j = json::parse(message);
                msg = j;
                std::printf("Section: %s - MsgType: %s\nData: %s\n", msg.section.c_str(), msg.msgType.c_str(), msg.data.c_str());
            }
            catch (std::exception &e)
            {
                std::cout << "Failed to convert the message: " << std::endl
                          << e.what() << std::endl;
            }

            if (msg.msgType == "partial" && msg.section == "config")
            {
                mck::Config _config;
                json j = json::parse(msg.data);
                try
                {
                    _config = j;
                    m_mixer.SetConfig(_config);
                }
                catch (std::exception &e)
                {
                    std::cout << "Failed to convert the config: " << std::endl
                              << e.what() << std::endl;
                    m_mixer.GetConfig(_config);
                }

                mck::Message outMsg("config", "partial");
                json jC = _config;
                outMsg.data = jC.dump();
                json jOut = outMsg;
                ws->send(jOut.dump(), uWS::OpCode::TEXT);
            } else if (msg.msgType == "command" && msg.section == "channel") {
                json j = json::parse(msg.data);
                mck::ChannelCommand cc;
                mck::Config config;
                try {
                    cc = j;
                    if (cc.command == "add") {
                        m_mixer.AddChannel(cc.isStereo, config);
                    } else if (cc.command == "remove") {
                        m_mixer.RemoveChannel(cc.idx, config);
                    } else {
                        return;
                    }

                    mck::Message outMsg("config", "partial");
                    json jC = config;
                    outMsg.data = jC.dump();
                    json jOut = outMsg;
                    ws->send(jOut.dump(), uWS::OpCode::TEXT);

                } catch (std::exception &e) {
                    std::fprintf(stderr, "Failed to read channel command: %s\n", e.what());
                }
            } },
                                  .drain = [](auto *ws) {
            /* Check ws->getBufferedAmount() here */ },
                                  .ping = [](auto *ws) {
            /* Not implemented yet */ },
                                  .pong = [](auto *ws) {
            /* Not implemented yet */ },
                                  .close = [](auto *ws, int code, std::string_view message) {
            /* You may access ws->getUserData() here */ }})
        .listen(9001, [](auto *token) {
            if (token)
            {
                std::cout << "Listening on port " << 9001 << std::endl;
            }
        })
        .run();

    m_mixer.Close();

    return 0;
}