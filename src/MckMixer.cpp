#include "MckMixer.h"

static int process(jack_nframes_t nframes, void *arg)
{
    ((mck::Mixer *)arg)->ProcessAudio(nframes);
    return 0;
}

mck::Mixer::Mixer()
    : m_isInitialized(false), m_bufferSize(0), m_sampleRate(0), m_activeConfig(0), m_newConfig(1), m_nInputChans()
{
    m_nInputChans[0] = 0;
    m_nInputChans[1] = 0;
    m_updateValues = false;
    m_updateCount = false;
    m_updateReady = false;
    m_isProcessing = true;
    m_phase = PROC_FADE_IN;
}

bool mck::Mixer::Init(std::string path)
{
    if (m_isInitialized)
    {
        std::fprintf(stderr, "MixerModule is already initialize\n");
        return false;
    }

    // Malloc Audio Inputs
    /*
    m_audioIn = (jack_port_t **)malloc(MCK_MIXER_MAX_INPUTS * sizeof(jack_port_t *));
    memset(m_audioIn, 0, MCK_MIXER_MAX_INPUTS * sizeof(jack_port_t *));
    m_bufferIn = (jack_default_audio_sample_t **)malloc(MCK_MIXER_MAX_INPUTS * sizeof(jack_nframes_t *));
    memset(m_bufferIn, 0, MCK_MIXER_MAX_INPUTS * sizeof(jack_nframes_t *));
    */

    // Open JACK server
    m_client = jack_client_open("MckMixer", JackNoStartServer, NULL);

    if (m_client == nullptr)
    {
        std::fprintf(stderr, "JACK server is not running.\n");
        return false;
    }

    jack_set_process_callback(m_client, process, (void *)this);

    // Set Output Channels
    m_audioOut[0] = jack_port_register(m_client, "audio_out_l", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    m_audioOut[1] = jack_port_register(m_client, "audio_out_r", JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);

    m_bufferSize = jack_get_buffer_size(m_client);
    m_sampleRate = jack_get_sample_rate(m_client);

    // Init Input DSP structures
    unsigned memSize = m_bufferSize * sizeof(jack_default_audio_sample_t);
    m_inputDsp = (InputDsp *)malloc(MCK_MIXER_MAX_INPUTS * sizeof(InputDsp));
    memset(m_inputDsp, 0, MCK_MIXER_MAX_INPUTS * sizeof(MCK_MIXER_MAX_INPUTS));
    /*
    for (unsigned i = 0; i < MCK_MIXER_MAX_INPUTS; i++)
    {
        m_inputDsp[i].buffer[0] = (jack_default_audio_sample_t *)malloc(memSize);
        m_inputDsp[i].buffer[1] = (jack_default_audio_sample_t *)malloc(memSize);
        memset(m_inputDsp[i].buffer[0], 0, memSize);
        memset(m_inputDsp[i].buffer[1], 0, memSize);
    }*/

    // Init DSP stuff
    m_interpolLin = (double *)malloc(m_bufferSize * sizeof(double));
    m_interpolSqrt = (double *)malloc(m_bufferSize * sizeof(double));
    for (unsigned i = 0; i < m_bufferSize; i++)
    {
        m_interpolLin[i] = (double)i / (double)(m_bufferSize - 1);
        m_interpolSqrt[i] = std::sqrt(m_interpolLin[i]);
    }

    m_reverbBuffer[0] = (jack_default_audio_sample_t *)malloc(m_bufferSize * sizeof(jack_default_audio_sample_t));
    m_reverbBuffer[1] = (jack_default_audio_sample_t *)malloc(m_bufferSize * sizeof(jack_default_audio_sample_t));
    memset(m_reverbBuffer[0], 0, m_bufferSize * sizeof(jack_default_audio_sample_t));
    memset(m_reverbBuffer[1], 0, m_bufferSize * sizeof(jack_default_audio_sample_t));

    m_delayBuffer[0] = (jack_default_audio_sample_t *)malloc(m_bufferSize * sizeof(jack_default_audio_sample_t));
    m_delayBuffer[1] = (jack_default_audio_sample_t *)malloc(m_bufferSize * sizeof(jack_default_audio_sample_t));
    memset(m_delayBuffer[0], 0, m_bufferSize * sizeof(jack_default_audio_sample_t));
    memset(m_delayBuffer[1], 0, m_bufferSize * sizeof(jack_default_audio_sample_t));

    // Reverb
    m_reverb = (fv3::revbase_f **)malloc(REV_LENGTH * sizeof(fv3::revbase_f *));
    m_reverb[REV_STREV] = new fv3::strev_f();
    m_reverb[REV_PROG] = new fv3::progenitor2_f();
    m_reverb[REV_ZREV] = new fv3::zrev2_f();
    m_reverb[REV_NREV] = new fv3::nrevb_f();
    for (unsigned i = 0; i < REV_LENGTH; i++)
    {
        m_reverb[i]->setSampleRate(m_sampleRate);
        m_reverb[i]->setwet(0.0);
        m_reverb[i]->setdry(-200.0);
    }

    // Delay
    m_delay.Init(m_sampleRate, m_bufferSize);

    // Read Configuration
    mck::Config newConfig;
    bool createFile = LoadConfig(newConfig, path) == false;

    if (createFile)
    {
        SaveConfig(newConfig, path);
    }

    m_path = fs::path(path);

    if (jack_activate(m_client))
    {
        std::fprintf(stderr, "Unable to activate JACK client!\n");
        return false;
    }

    SetConfig(newConfig);

    m_isInitialized = true;
    return true;
}

void mck::Mixer::Close()
{
    if (m_isInitialized == false)
    {
        return;
    }

    if (m_client != nullptr)
    {
        m_phase = PROC_CLOSING;
        std::unique_lock<std::mutex> lck(m_updateMutex);
        while (true)
        {
            if (m_phase.load() == PROC_CLOSED)
            {
                break;
            }
            m_updateCond.wait(lck);
        }

        // Save Connections here
        std::vector<std::string> cons;
        mck::GetConnections(m_client, m_audioOut[0], cons);
        m_config[m_activeConfig].targetLeft = cons;

        mck::GetConnections(m_client, m_audioOut[1], cons);
        m_config[m_activeConfig].targetRight = cons;

        // Save Input Connections
        for (unsigned i = 0; i < m_config[m_activeConfig].channelCount; i++)
        {
            mck::GetConnections(m_client, m_inputDsp[i].port[0], cons);
            if (cons.size() > 0)
            {
                m_config[m_activeConfig].channels[i].sourceLeft = cons[0];
            }
            else
            {
                m_config[m_activeConfig].channels[i].sourceLeft = "";
            }
            if (m_config[m_activeConfig].channels[i].isStereo)
            {
                mck::GetConnections(m_client, m_inputDsp[i].port[1], cons);
                if (cons.size() > 0)
                {
                    m_config[m_activeConfig].channels[i].sourceRight = cons[0];
                }
                else
                {
                    m_config[m_activeConfig].channels[i].sourceRight = "";
                }
            }
        }
        try
        {
            jack_client_close(m_client);
        }
        catch (std::exception &e)
        {
            std::fprintf(stderr, "Failed to close JACK client: %s\n", e.what());
        }
    }

    SaveConfig(m_config[m_activeConfig], m_path.string());

    free(m_inputDsp);

    free(m_interpolLin);
    free(m_interpolSqrt);

    free(m_reverbBuffer[0]);
    free(m_reverbBuffer[1]);
    free(m_delayBuffer[0]);
    free(m_delayBuffer[1]);

    for (unsigned i = 0; i < REV_LENGTH; i++)
    {
        delete m_reverb[i];
    }
    free(m_reverb);

    m_isInitialized = false;
}

bool mck::Mixer::SetConfig(mck::Config &config)
{
    if (m_phase.load() == PROC_UPDATING)
    {
        std::fprintf(stderr, "MckMixer is updating...\n");
        return false;
    }

    unsigned nChans = 0;
    m_newConfig = 1 - m_activeConfig;

    // Convert Gain dB to lin
    config.gainLin = mck::DbToLin(config.gain);
    config.reverb.gainLin = mck::DbToLin(config.reverb.gain);
    config.delay.gainLin = mck::DbToLin(config.delay.gain);

    for (unsigned i = 0; i < config.channels.size(); i++)
    {
        if (config.channels[i].isStereo)
        {
            nChans += 2;
        }
        else
        {
            nChans += 1;
        }

        config.channels[i].pan = std::min(100.0, std::max(0.0, config.channels[i].pan));

        config.channels[i].gainLin = mck::DbToLin(config.channels[i].gain);
        config.channels[i].sendReverbLin = mck::DbToLin(config.channels[i].sendReverb);
        config.channels[i].sendDelayLin = mck::DbToLin(config.channels[i].sendDelay);
    }
    config.reverb.type = std::min(config.reverb.type, (unsigned)(REV_LENGTH - 1));
    config.channelCount = config.channels.size();

    // Connect Output Channels
    if (mck::NewConnections(m_client, m_audioOut[0], config.targetLeft))
    {
        mck::SetConnections(m_client, m_audioOut[0], config.targetLeft, false);
    }
    if (mck::NewConnections(m_client, m_audioOut[1], config.targetRight))
    {
        mck::SetConnections(m_client, m_audioOut[1], config.targetRight, false);
    }

    m_config[m_newConfig] = config;
    m_nInputChans[m_newConfig] = nChans;

    std::string name = "";

    if (nChans != m_nInputChans[m_activeConfig] || config.channelCount != m_config[m_activeConfig].channelCount)
    {
        mck::Config &curConfig = m_config[m_activeConfig];

        // Signal audio process to fade out and wait for new values
        m_phase = PROC_FADE_OUT;
        char phase = PROC_FADE_OUT;
        std::unique_lock<std::mutex> lck(m_updateMutex);
        while (true)
        {
            phase = m_phase.load();
            if (phase == PROC_CLOSED)
            {
                return false;
            }
            if (phase == PROC_BYPASS)
            {
                break;
            }
            m_updateCond.wait(lck);
        }
        int ret = 0;
        //ret = jack_deactivate(m_client);

        // Change exisiting channels
        for (unsigned i = 0; i < std::min(config.channelCount, curConfig.channelCount); i++)
        {
            if (config.channels[i].isStereo != curConfig.channels[i].isStereo)
            {
                if (config.channels[i].isStereo)
                {
                    name = "audio_in_" + std::to_string(i + 1) + "_l";
                    jack_port_rename(m_client, m_inputDsp[i].port[0], name.c_str());
                    name = "audio_in_" + std::to_string(i + 1) + "_r";
                    m_inputDsp[i].port[1] = jack_port_register(m_client, name.c_str(), JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
                    m_inputDsp[i].isStereo = true;
                }
                else
                {
                    name = "audio_in_" + std::to_string(i + 1) + "_m";
                    jack_port_rename(m_client, m_inputDsp[i].port[0], name.c_str());
                    jack_port_unregister(m_client, m_inputDsp[i].port[1]);
                    m_inputDsp[i].isStereo = false;
                }
            }
        }
        // Delete existing channels
        for (unsigned i = config.channelCount; i < curConfig.channelCount; i++)
        {
            jack_port_unregister(m_client, m_inputDsp[i].port[0]);
            if (m_inputDsp[i].isStereo)
            {
                jack_port_unregister(m_client, m_inputDsp[i].port[1]);
            }
        }

        // Add new channels
        for (unsigned i = curConfig.channelCount; i < config.channelCount; i++)
        {
            if (config.channels[i].isStereo)
            {
                name = "audio_in_" + std::to_string(i + 1) + "_l";
                m_inputDsp[i].port[0] = jack_port_register(m_client, name.c_str(), JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
                name = "audio_in_" + std::to_string(i + 1) + "_r";
                m_inputDsp[i].port[1] = jack_port_register(m_client, name.c_str(), JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
                m_inputDsp[i].isStereo = true;
            }
            else
            {
                name = "audio_in_" + std::to_string(i + 1) + "_m";
                m_inputDsp[i].port[0] = jack_port_register(m_client, name.c_str(), JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
                m_inputDsp[i].isStereo = false;
            }
        }

        // Check Connections
        std::vector<std::string> cons;
        cons.push_back("");
        for (unsigned i = 0; i < config.channelCount; i++)
        {
            mck::SetConnection(m_client, m_inputDsp[i].port[0], config.channels[i].sourceLeft, true);
            if (config.channels[i].isStereo)
            {
                mck::SetConnection(m_client, m_inputDsp[i].port[1], config.channels[i].sourceRight, true);
            }
        }

        m_phase = PROC_FADE_IN;

        //jack_activate(m_client);

        SaveConfig(config, m_path.string());

        return true;
    }
    else
    {
        m_phase = PROC_UPDATING;

        SaveConfig(config, m_path.string());

        return true;
    }
}

bool mck::Mixer::GetConfig(mck::Config &config)
{
    config = m_config[m_activeConfig];
    return true;
}

bool mck::Mixer::AddChannel(bool isStereo, mck::Config &outConfig)
{
    if (m_nInputChans[m_activeConfig] == 32 || (m_nInputChans[m_activeConfig] == 31 && isStereo))
    {
        return false;
    }

    mck::Channel chan;

    GetConfig(outConfig);

    chan.name = "Channel " + std::to_string(outConfig.channels.size() + 1);
    chan.isStereo = isStereo;

    outConfig.channels.push_back(chan);

    bool ret = SetConfig(outConfig);

    if (ret == false)
    {
        GetConfig(outConfig);
    }

    return ret;
}

bool mck::Mixer::RemoveChannel(unsigned idx, mck::Config &outConfig)
{
    GetConfig(outConfig);

    if (idx >= outConfig.channelCount)
    {
        return false;
    }

    outConfig.channels.erase(outConfig.channels.begin() + idx);

    bool ret = SetConfig(outConfig);
    if (ret == false)
    {
        GetConfig(outConfig);
    }

    return ret;
}

bool mck::Mixer::ApplyConnectionCommand(mck::ConnectionCommand cmd, mck::Config &outConfig)
{
    std::vector<std::string> cons;
    GetConfig(outConfig);

    if (cmd.isInput)
    {
        if (cmd.idx >= outConfig.channelCount)
        {
            return false;
        }

        if (outConfig.channels[cmd.idx].isStereo)
        {
            if (cmd.subIdx > 1) {
                return false;
            }
        }
        else if (cmd.subIdx > 0)
        {
            return false;
        }

        if (cmd.command == "connect")
        {
            cons.push_back(cmd.target);

            if (mck::NewConnections(m_client, m_inputDsp[cmd.idx].port[cmd.subIdx], cons))
            {
                if (mck::SetConnections(m_client, m_inputDsp[cmd.idx].port[cmd.subIdx], cons, true) == false)
                {
                    return false;
                }
                if (cmd.subIdx == 0)
                {
                    outConfig.channels[cmd.idx].sourceLeft = cons[0];
                }
                else
                {
                    outConfig.channels[cmd.idx].sourceRight = cons[0];
                }
                return SetConfig(outConfig);
            }
        }
        else if (cmd.command == "disconnect")
        {
            if (mck::SetConnections(m_client, m_inputDsp[cmd.idx].port[cmd.subIdx], cons, true) == false)
            {
                return false;
            }
            if (cmd.subIdx == 0)
            {
                outConfig.channels[cmd.idx].sourceLeft = "";
            }
            else
            {
                outConfig.channels[cmd.idx].sourceRight = "";
            }
            return SetConfig(outConfig);
        }
    }
    else
    {
        if (cmd.subIdx > 1)
        {
            return false;
        }

        if (cmd.command == "connect")
        {
            cons.push_back(cmd.target);

            if (mck::NewConnections(m_client, m_audioOut[cmd.subIdx], cons))
            {
                if (mck::SetConnections(m_client, m_audioOut[cmd.subIdx], cons, true) == false)
                {
                    return false;
                }
                if (cmd.subIdx == 0)
                {
                    outConfig.targetLeft = cons;
                }
                else
                {
                    outConfig.targetRight = cons;
                }
                return SetConfig(outConfig);
            }
        }
        else if (cmd.command == "disconnect")
        {
            if (mck::SetConnections(m_client, m_audioOut[cmd.subIdx], cons, true) == false)
            {
                return false;
            }
            if (cmd.subIdx == 0)
            {
                outConfig.targetLeft = cons;
            }
            else
            {
                outConfig.targetRight = cons;
            }
            return SetConfig(outConfig);
        }
    }

    return true;
}

void mck::Mixer::ProcessAudio(jack_nframes_t nframes)
{
    char phase = m_phase.load();

    if (phase == PROC_CLOSED)
    {
        return;
    }

    // Output Channels
    m_bufferOut[0] = (jack_default_audio_sample_t *)jack_port_get_buffer(m_audioOut[0], nframes);
    m_bufferOut[1] = (jack_default_audio_sample_t *)jack_port_get_buffer(m_audioOut[1], nframes);

    // Reset
    memset(m_bufferOut[0], 0, nframes * sizeof(jack_default_audio_sample_t));
    memset(m_bufferOut[1], 0, nframes * sizeof(jack_default_audio_sample_t));

    if (phase == PROC_BYPASS)
    {
        return;
    }

    memset(m_reverbBuffer[0], 0, m_bufferSize * sizeof(jack_default_audio_sample_t));
    memset(m_reverbBuffer[1], 0, m_bufferSize * sizeof(jack_default_audio_sample_t));

    memset(m_delayBuffer[0], 0, m_bufferSize * sizeof(jack_default_audio_sample_t));
    memset(m_delayBuffer[1], 0, m_bufferSize * sizeof(jack_default_audio_sample_t));

    // Input Channels
    for (unsigned i = 0; i < m_config[m_activeConfig].channelCount; i++)
    {
        m_inputDsp[i].buffer[0] = (jack_default_audio_sample_t *)jack_port_get_buffer(m_inputDsp[i].port[0], nframes);
        if (m_inputDsp[i].isStereo)
        {
            m_inputDsp[i].buffer[1] = (jack_default_audio_sample_t *)jack_port_get_buffer(m_inputDsp[i].port[1], nframes);
        }
    }

    double updateGain;

    switch (phase)
    {
    case PROC_FADE_OUT:
    case PROC_CLOSING:
        // Fade Out old dsp
        for (unsigned i = 0; i < m_config[m_activeConfig].channelCount; i++)
        {
            for (unsigned s = 0; s < nframes; s++)
            {
                m_inputDsp[i].buffer[0][s] = m_interpolSqrt[nframes - s - 1] * m_config[m_activeConfig].channels[i].gainLin * m_inputDsp[i].buffer[0][s];
                if (m_config[m_activeConfig].channels[i].isStereo)
                {
                    m_inputDsp[i].buffer[1][s] = m_interpolSqrt[nframes - s - 1] * m_config[m_activeConfig].channels[i].gainLin * m_inputDsp[i].buffer[1][s];
                }
            }
        }
        break;
    case PROC_FADE_IN:
        // Fade In new dsp
        for (unsigned i = 0; i < m_config[m_activeConfig].channelCount; i++)
        {
            for (unsigned s = 0; s < nframes; s++)
            {
                m_inputDsp[i].buffer[0][s] = m_interpolSqrt[s] * m_config[m_activeConfig].channels[i].gainLin * m_inputDsp[i].buffer[0][s];
                if (m_config[m_activeConfig].channels[i].isStereo)
                {
                    m_inputDsp[i].buffer[1][s] = m_interpolSqrt[s] * m_config[m_activeConfig].channels[i].gainLin * m_inputDsp[i].buffer[1][s];
                }
            }
        }
        break;
    case PROC_UPDATING:
        // Apply new values
        for (unsigned i = 0; i < m_config[m_activeConfig].channelCount; i++)
        {
            for (unsigned s = 0; s < nframes; s++)
            {
                m_inputDsp[i].buffer[0][s] = (m_interpolLin[s] * m_config[m_newConfig].channels[i].gainLin + m_interpolLin[nframes - s - 1] * m_config[m_activeConfig].channels[i].gainLin) * m_inputDsp[i].buffer[0][s];
                if (m_config[m_activeConfig].channels[i].isStereo)
                {
                    m_inputDsp[i].buffer[1][s] = (m_interpolLin[s] * m_config[m_newConfig].channels[i].gainLin + m_interpolLin[nframes - s - 1] * m_config[m_activeConfig].channels[i].gainLin) * m_inputDsp[i].buffer[1][s];
                }
            }
        }
        break;
    case PROC_NORMAL:
    default:
        // Normal processing
        for (unsigned i = 0; i < m_config[m_activeConfig].channelCount; i++)
        {
            for (unsigned s = 0; s < nframes; s++)
            {
                m_inputDsp[i].buffer[0][s] = m_config[m_activeConfig].channels[i].gainLin * m_inputDsp[i].buffer[0][s];
                if (m_config[m_activeConfig].channels[i].isStereo)
                {
                    m_inputDsp[i].buffer[1][s] = m_config[m_activeConfig].channels[i].gainLin * m_inputDsp[i].buffer[1][s];
                }
            }
        }
        break;
    }

    // Mix Inputs to Output
    double panL = 0.0;
    double panR = 0.0;
    double revSend = 0.0;
    double dlySend = 0.0;
    for (unsigned i = 0; i < m_config[m_activeConfig].channelCount; i++)
    {
        panR = std::sqrt(m_config[m_activeConfig].channels[i].pan / 100.0);
        panL = std::sqrt(1.0 - m_config[m_activeConfig].channels[i].pan / 100.0);
        revSend = m_config[m_activeConfig].channels[i].sendReverbLin;
        dlySend = m_config[m_activeConfig].channels[i].sendDelayLin;

        if (m_config[m_activeConfig].channels[i].isStereo)
        {
            for (unsigned s = 0; s < nframes; s++)
            {
                // Output
                m_bufferOut[0][s] += m_config[m_activeConfig].gainLin * panL * m_inputDsp[i].buffer[0][s];
                m_bufferOut[1][s] += m_config[m_activeConfig].gainLin * panR * m_inputDsp[i].buffer[1][s];

                // Reverb
                m_reverbBuffer[0][s] += revSend * panL * m_inputDsp[i].buffer[0][s];
                m_reverbBuffer[1][s] += revSend * panR * m_inputDsp[i].buffer[1][s];

                // Delay
                m_delayBuffer[0][s] += dlySend * panL * m_inputDsp[i].buffer[0][s];
                m_delayBuffer[1][s] += dlySend * panR * m_inputDsp[i].buffer[1][s];
            }
        }
        else
        {
            for (unsigned s = 0; s < nframes; s++)
            {
                // Output
                m_bufferOut[0][s] += m_config[m_activeConfig].gainLin * panL * m_inputDsp[i].buffer[0][s];
                m_bufferOut[1][s] += m_config[m_activeConfig].gainLin * panR * m_inputDsp[i].buffer[0][s];

                // Reverb
                m_reverbBuffer[0][s] += revSend * panL * m_inputDsp[i].buffer[0][s];
                m_reverbBuffer[1][s] += revSend * panR * m_inputDsp[i].buffer[0][s];

                // Delay
                m_delayBuffer[0][s] += dlySend * panL * m_inputDsp[i].buffer[0][s];
                m_delayBuffer[1][s] += dlySend * panR * m_inputDsp[i].buffer[0][s];
            }
        }
    }

    // Reverb Processing
    ProcessReverb(nframes, m_config[m_activeConfig].reverb.rt60, m_config[m_activeConfig].reverb.type);

    // Delay Processing
    ProcessDelay(nframes, m_config[m_activeConfig].delay.delay, m_config[m_activeConfig].delay.feedback);

    for (unsigned s = 0; s < nframes; s++)
    {
        m_bufferOut[0][s] += m_config[m_activeConfig].gainLin * m_config[m_activeConfig].reverb.gainLin * m_reverbBuffer[0][s];
        m_bufferOut[1][s] += m_config[m_activeConfig].gainLin * m_config[m_activeConfig].reverb.gainLin * m_reverbBuffer[1][s];

        m_bufferOut[0][s] += m_config[m_activeConfig].gainLin * m_config[m_activeConfig].delay.gainLin * m_delayBuffer[0][s];
        m_bufferOut[1][s] += m_config[m_activeConfig].gainLin * m_config[m_activeConfig].delay.gainLin * m_delayBuffer[1][s];
    }

    switch (phase)
    {
    case PROC_UPDATING:
        m_phase = PROC_NORMAL;
        m_activeConfig = m_newConfig;
        break;
    case PROC_FADE_OUT:
        m_phase = PROC_BYPASS;
        m_activeConfig = m_newConfig;
        m_updateCond.notify_all();
        break;
    case PROC_FADE_IN:
        m_phase = PROC_NORMAL;
        break;
    case PROC_CLOSING:
        m_phase = PROC_CLOSED;
        m_updateCond.notify_all();
        break;
    default:
        break;
    }
}

void mck::Mixer::ProcessReverb(jack_nframes_t nframes, float rt60, unsigned type)
{
    if (type >= REV_LENGTH)
    {
        return;
    }
    switch (type)
    {
    case REV_STREV:
        if (((fv3::strev_f *)m_reverb[type])->getrt60() != rt60)
        {
            ((fv3::strev_f *)m_reverb[type])->setrt60(rt60);
        }
        break;
    case REV_PROG:
        if (((fv3::progenitor2_f *)m_reverb[type])->getrt60() != rt60)
        {
            ((fv3::progenitor2_f *)m_reverb[type])->setrt60(rt60);
        }
        break;
    case REV_ZREV:
        if (((fv3::zrev2_f *)m_reverb[type])->getrt60() != rt60)
        {
            ((fv3::zrev2_f *)m_reverb[type])->setrt60(rt60);
        }
        break;
    case REV_NREV:
        if (((fv3::nrevb_f *)m_reverb[type])->getrt60() != rt60)
        {
            ((fv3::nrevb_f *)m_reverb[type])->setrt60(rt60);
        }
        break;
    default:
        return;
    }
    m_reverb[type]->processreplace(m_reverbBuffer[0], m_reverbBuffer[1], m_reverbBuffer[0], m_reverbBuffer[1], nframes);
}

void mck::Mixer::ProcessDelay(jack_nframes_t nframes, double delay, double feedback)
{
    if (delay != m_delay.GetDelayTime())
    {
        m_delay.SetDelayTime(delay);
    }

    if (feedback != m_delay.GetFeedback())
    {
        m_delay.SetFeedback(feedback);
    }

    m_delay.ProcessAudio(m_delayBuffer[0], m_delayBuffer[1]);
}

// File Handling
bool mck::Mixer::LoadConfig(mck::Config &config, std::string path)
{
    if (fs::exists(path))
    {
        std::ifstream configFile(path);
        json j;
        configFile >> j;
        configFile.close();
        try
        {
            config = j;
            return true;
        }
        catch (std::exception &e)
        {
            std::fprintf(stderr, "Failed to read the config file: %s\n", e.what());
        }
    }
    return false;
}
bool mck::Mixer::SaveConfig(mck::Config &config, std::string path)
{
    fs::path p(path);
    // Save Configuration
    std::ofstream configFile(path);
    json j = config;
    configFile << std::setw(4) << j << std::endl;
    configFile.close();
    return true;
}