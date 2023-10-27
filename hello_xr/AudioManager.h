//
// Created by User1 on 2022/10/29.
//

#ifndef HELLOXR_AUDIOMANAGER_H
#define HELLOXR_AUDIOMANAGER_H

#pragma once

#include <AL/alut.h>
#include <AL/alext.h>
#include <set>
#include "utils.hpp"
#include "Game.hpp"

class SoundEffect {
    ALuint handle;
public:
    SoundEffect(const std::string& path) {
        auto fDat = file_get_contents(path);
        handle = alutCreateBufferFromFileImage(fDat.data(), fDat.size());
    }
//    SoundEffect(const std::string& path) : handle(alutCreateBufferFromFile(path.c_str())) {}
    ~SoundEffect() {
        alDeleteBuffers(1, &handle);
    }
    auto get() const {
        return handle;
    }
};
class AudioSource {
    ALuint handle = -1;
public:
    AudioSource(glm::vec3 pos){
        alGenSources(1, &handle);
        alSource3f(handle, AL_POSITION, pos.x, pos.y, pos.z);
    }
    AudioSource(AudioSource&& o) {
        this->handle = o.handle;
        o.handle = -1;
    }
    auto& operator=(AudioSource&& o) {
        this->handle = o.handle;
        o.handle = -1;
        return *this;
    }
    ~AudioSource() {
        if (handle == -1)
            return;
        alSourceStop(handle);
        alDeleteSources(1, &handle);
    }

    void setPos(const glm::vec3& pos) const {
        alSource3f(handle, AL_POSITION, pos.x, pos.y, pos.z);
    }
    void play(const SoundEffect& se) const {
        alSourceStop(handle);
        alSourcei(handle, AL_BUFFER, se.get());
        alSourcePlay(handle);
    }
};

class OneShotAudioManager {
    struct OneShotAudio {
        AudioSource src;
        double ttl;
        OneShotAudio() : src(glm::vec3{}), ttl(2.0f) {}
    };
    std::vector<OneShotAudio> container;
    std::set<int> available;
public:
    OneShotAudioManager(size_t slotnum) {
        container.resize(slotnum);
        for (int i = 0; i < slotnum; i++)
            available.insert(i);
    }
    void play(const SoundEffect& se, const glm::vec3& pos) {
        if (available.empty()) {
            std::cerr << "warning: sound effect slot not available" << std::endl;
            return;
        }
        auto availableIndex = *available.begin();
        available.erase(availableIndex);
        auto& audioSlot = container[availableIndex];
        audioSlot.src.setPos(pos);
        audioSlot.src.play(se);
        audioSlot.ttl = 2.0;
        //std::cout << "sount slot " << availableIndex << std::endl;
    }

    void update(double dt) {
        for (int i = 0; i < container.size(); i++) {
            auto& audioSlot = container[i];
            if (audioSlot.ttl > 0) {
                audioSlot.ttl -= dt;
                if (audioSlot.ttl <= 0) {
                    available.insert(i);
                }
            }
        }
    }
};


class OpenALManager {
    ALCcontext* ctx;
    ALCdevice* device;

    LPALCGETSTRINGISOFT alcGetStringiSOFT;
    LPALCRESETDEVICESOFT alcResetDeviceSOFT;
public:
    OpenALManager() {
        if (!alutInit(nullptr, nullptr)) {
            throw std::runtime_error("failed to initialize alut");
        }

        ctx = alcGetCurrentContext();
        device = alcGetContextsDevice(ctx);

#define LOAD_PROC(d, T, x)  ((x) = reinterpret_cast<T>(alcGetProcAddress((d), #x)))
        LOAD_PROC(device, LPALCGETSTRINGISOFT, alcGetStringiSOFT);
        LOAD_PROC(device, LPALCRESETDEVICESOFT, alcResetDeviceSOFT);
#undef LOAD_PROC

        ALCint num_hrtf;
        alcGetIntegerv(device, ALC_NUM_HRTF_SPECIFIERS_SOFT, 1, &num_hrtf);
        if (!num_hrtf)
            printf("No HRTFs found\n");
        else
        {
            ALCint attr[5];
            ALCint index = -1;
            ALCint i;

            printf("Available HRTFs:\n");
            for (i = 0; i < num_hrtf; i++)
            {
                const ALCchar* name = alcGetStringiSOFT(device, ALC_HRTF_SPECIFIER_SOFT, i);
                printf("    %d: %s\n", i, name);
            }

            i = 0;
            attr[i++] = ALC_HRTF_SOFT;
            attr[i++] = ALC_TRUE;
            if (index == -1)
            {
                printf("Using default HRTF...\n");
            }
            else
            {
                printf("Selecting HRTF %d...\n", index);
                attr[i++] = ALC_HRTF_ID_SOFT;
                attr[i++] = index;
            }
            attr[i] = 0;

            if (!alcResetDeviceSOFT(device, attr))
                printf("Failed to reset device: %s\n", alcGetString(device, alcGetError(device)));
        }
    }
    ~OpenALManager() {
        alutExit();
    }

    auto setListenerPose(const Game::Pose& pose) const {
        alListener3f(AL_POSITION, pose.pos.x, pose.pos.y, pose.pos.z);

        auto lookTo = pose.ori * glm::vec3{ 0, 0, -1 };
        auto up = pose.ori* glm::vec3{ 0, 1, 0 };
        float v[6] = { lookTo.x, lookTo.y, lookTo.z, up.x, up.y, up.z };

        alListenerfv(AL_ORIENTATION, v);
    }
};

#endif //HELLOXR_AUDIOMANAGER_H
