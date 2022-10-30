#include <iostream>
#include <queue>
#include <cmath>
#include <fmt/format.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/transform.hpp>
#include <glm/gtx/intersect.hpp>

#include "AudioManager.h"

#include "Game.hpp"
#include "utils.hpp"

namespace Game {

    constexpr double pi = 3.141592653589793238462643383279;

    std::optional<OpenALManager> alManager;

    ModelHandle testModel, gunModel, beamModel, tgtModel, gamestartModel, gamestartSelectedModel, sphereModel, scoreModel[4], numberModel[10], timeModel, scoreStrModel;
    std::optional<AudioSource> gunAudioSrc[2];
    std::optional<SoundEffect> gunSe, tgtSe, cntSe, buzzerSe;
    std::optional<OneShotAudioManager> seManager;

    bool initialized = false;

    constexpr int handPoseBufSize = 3;
    int handPoseBufIndex[2] = {};
    std::optional<Game::Pose> handPoseBuf[2][handPoseBufSize];
    std::optional<Game::Pose> handPose[2];

    Pose stagePose;
    glm::vec3 fwdVec, upperVec, rightVec;
    Pose sightBase;

    Pose gameStartStrPose;

    double stageRotate = 0.0;
    double gameTimer = 0.0;
    double tgtTimer = 0.0;
    int score;

    bool gsSelected = false;
    bool gsSelectedHands[2] = {false, false};
    std::optional<double> rayD[2];

    enum class Scene {
        Title, MainGame, ScoreResult,
    };
    Scene scene = Scene::Title;

    GameData gameDat;
    bool trigger_old[2];

    template<typename V, typename T>
    auto vLerp(V v1, V v2, T t) {
        return T(1.0 - t) * v1 + t * v2;
    }

    struct Rect {
        Pose pose;
        double w, h;

        std::optional<std::pair<glm::vec3, float>> RayCast(glm::vec3 rayfrom, glm::vec3 rayDir) {
            float d;
            auto norm = pose.ori * glm::vec3{ 0,0,1 };
            if (glm::dot(rayDir, norm) >= 0 && glm::intersectRayPlane(rayfrom, rayDir, pose.pos, norm, d)) {
                auto p = rayfrom + d * rayDir;

                auto pPlane = p - pose.pos;

                auto xVec = pose.ori * glm::vec3{ 1,0,0 };
                auto yVec = pose.ori * glm::vec3{ 0,1,0 };

                if (std::abs(glm::dot(xVec, pPlane)) < w / 2 && std::abs(glm::dot(yVec, pPlane)) < h / 2) {
                    return std::make_pair(p, d);
                }
                else {
                    return std::nullopt;
                }
            }
            else
                return std::nullopt;
        }
    };

    struct Target {
        Pose pose;
        Pose tgtPose;
        Pose endPose;
        Pose hidPose;
        double timer = 0.0;
        bool alive = true;

        float angleA, angleB, dist;

        Target() {
            angleA = rand() % 1000 * 0.002 - 1;
            angleB = rand() % 1000 * 0.002 - 1;
            dist = rand() % 1000 * 0.007 + 1;

            tgtPose.pos = sightBase.pos + dist * (upperVec * std::sin(angleA) + rightVec * std::cos(angleA) * std::sin(angleB) + fwdVec * std::cos(angleA) * std::cos(angleB));
            tgtPose.ori = glm::rotate(glm::identity<glm::quat>(), float(angleA), rightVec)
                          * glm::rotate(glm::identity<glm::quat>(), float(-angleB), upperVec) * stagePose.ori
                          * glm::rotate(glm::identity<glm::quat>(), float(pi), glm::vec3{0, 1, 0})
                    ;
            tgtPose.ori = glm::normalize(tgtPose.ori);

            endPose = tgtPose;
            endPose.ori = glm::rotate(glm::identity<glm::quat>(), float(angleA), rightVec)
                          * glm::rotate(glm::identity<glm::quat>(), float(-angleB), upperVec) * stagePose.ori;
            endPose.ori = glm::normalize(endPose.ori);

            hidPose = tgtPose;
            hidPose.pos -= 10.0f * upperVec;

            pose = hidPose;
        }

        void proc(double dt) {
            if (timer < 2.0f) {
                double t = timer / 2.0;
                pose.pos = vLerp(hidPose.pos, tgtPose.pos, float(1.0 - (1.0 - t) * (1.0 - t)));
            }
            else if (timer < 4.0f) {
                pose = tgtPose;
            }
            else if (timer < 5.0f) {
                double t = timer - 4.0f;
                pose = tgtPose;
                pose.ori = glm::slerp(tgtPose.ori, endPose.ori, float(t));
                pose.ori = glm::normalize(pose.ori);
            }
            else if (timer < 7.0f) {
                double t = (timer - 5.0f) / 2.0;
                pose.pos = vLerp(tgtPose.pos, hidPose.pos, float(t * t));
            }
            else {
                alive = false;
            }

            timer += dt;
        }

        struct Hit {
            bool enable;
            glm::vec3 colPos;
            float rayLen;
            float dCenter;
        };

        std::optional<Hit> RayCast(glm::vec3 rayfrom, glm::vec3 rayDir) {
            float rayLen;
            auto norm = pose.ori * glm::vec3{ 0,0,1 };
            if (glm::intersectRayPlane(rayfrom, rayDir, pose.pos, norm, rayLen)) {
                auto p = rayfrom + rayLen * rayDir;

                auto pPlane = p - pose.pos;

                auto dCenter = glm::length(pPlane);

                if (dCenter <= 0.2) {
                    return Hit{ glm::dot(rayDir, norm) >= 0, p, rayLen, dCenter };
                }
                else {
                    return std::nullopt;
                }
            }
            else
                return std::nullopt;
        }
    };

    struct scoreEffect {
        int type;
        Pose pose;
        bool alive = true;
        double timer = 0;

        scoreEffect(glm::vec3 pos, int ty) {
            type = ty;
            pose.pos = pos;
            pose.ori = stagePose.ori * glm::rotate(glm::identity<glm::quat>(), float(pi), glm::vec3{ 0, 1, 0 });
        }
        void proc(double dt) {
            timer += dt;
            pose.pos += float(dt * 0.3) * upperVec;
            if (timer > 2.0) {
                alive = false;
            }
        }
        void draw(IGraphicsProvider& g) const {
            g.DrawModel(scoreModel[type], pose.pos, pose.ori, glm::vec3{ 0.2, 0.2, 0.2 });
        }
    };

    struct boomEffect {
        glm::vec3 pos, vel;
        double timer = 0;
        bool alive = true;
        boomEffect(glm::vec3 pos) : pos(pos) {
            float angle = float(rand() % 10000) / 5000 * pi;
            vel = 3.0f * (upperVec * sin(angle) + rightVec * cos(angle));
        }
        void proc(double dt) {
            timer += dt;
            pos += float(dt) * vel;
            if (timer > 0.2) {
                alive = false;
            }
        }
        void draw(IGraphicsProvider& g) const {
            g.DrawModel(sphereModel, pos, glm::quat(0,0,0,1), float(0.2 - timer) * glm::vec3{0.2, 0.2, 0.2});
        }
    };

    std::vector<Target> targets;
    std::vector<scoreEffect> scoreEffects;
    std::vector<boomEffect> boomEffects;

    void init(IGraphicsProvider& g) {
        testModel = g.LoadModel("testcube.glb");
        gunModel = g.LoadModel("gun.glb");
        beamModel = g.LoadModel("beam.glb");
        tgtModel = g.LoadModel("target.glb");
        gamestartModel = g.LoadModel("GameStart.glb");
        gamestartSelectedModel = g.LoadModel("GameStart_selected.glb");
        timeModel = g.LoadModel("time.glb");
        scoreStrModel = g.LoadModel("score.glb");
        sphereModel = g.LoadModel("sphere.glb");
        scoreModel[0] = g.LoadModel("score100.glb");
        scoreModel[1] = g.LoadModel("score50.glb");
        scoreModel[2] = g.LoadModel("score30.glb");
        scoreModel[3] = g.LoadModel("score10.glb");
        for (int i = 0; i < 10; i++) {
            auto tmp = fmt::format("{}.glb", i);
            numberModel[i] = g.LoadModel(tmp.c_str());
        }

        alManager.emplace();
        gunSe.emplace("gun.wav");
        tgtSe.emplace("target.wav");
        buzzerSe.emplace("buzzer.wav");
        cntSe.emplace("pi.wav");

        for (int i = 0; i < 2; i++)
            gunAudioSrc[i].emplace(glm::vec3{});

        seManager.emplace(128);
    }

    void proc_init(const GameData& dat) {
        if (dat.stagePose.has_value()) {
            stagePose = dat.stagePose.value();
            fwdVec = dat.stagePose->ori * glm::vec3(0, 0, -1);
            upperVec = dat.stagePose->ori * glm::vec3(0, 1, 0);
            rightVec = dat.stagePose->ori * glm::vec3(1, 0, 0);

            sightBase = stagePose;
            constexpr float sightHeight = 1.2;
            sightBase.pos += sightHeight * upperVec;

            gameStartStrPose = sightBase;
            gameStartStrPose.ori = glm::rotate(glm::identity<glm::quat>(), float(pi), glm::vec3{ 0, 1, 0 }) * gameStartStrPose.ori;
            gameStartStrPose.pos += fwdVec * 3.0f;

            tgtTimer = 1.0;

            initialized = true;
        }
    }

    void proc_main(const GameData& dat) {
        stageRotate += dat.dt * 0.1;
        if (stageRotate > 1.0f)
            stageRotate -= 1.0f;
        for (int i = 0; i < 2; i++)
            rayD[i].reset();

        for (int i = 0; i < 2; i++) {
            if (dat.handPoses[i].has_value()) {
                handPoseBuf[i][handPoseBufIndex[i]++] = dat.handPoses[i].value();
                handPoseBufIndex[i] %= handPoseBufSize;
            }

            bool ok = true;
            for (int j = 0; j < handPoseBufSize; j++) {
                if (!handPoseBuf[i][j].has_value())
                    ok = false;
            }
            if (ok) {
                handPose[i].emplace();
                for (int j = 0; j < handPoseBufSize; j++) {
                    handPose[i]->pos += handPoseBuf[i][j]->pos;
                }
                handPose[i]->pos /= float(handPoseBufSize);

                handPose[i]->ori = handPoseBuf[i][handPoseBufIndex[i]]->ori;
                for (int j = 1; j < handPoseBufSize; j++) {
                    handPose[i]->ori = glm::slerp(handPose[i]->ori, handPoseBuf[i][(handPoseBufIndex[i] + j) % handPoseBufSize]->ori, 1.0f / (j + 1));
                }
            }
            else {
                handPose[i] = std::nullopt;
            }
        }

        if(dat.viewPose.has_value())
            alManager->setListenerPose(dat.viewPose.value());
        for (int i = 0; i < 2; i++) {
            if (handPose[i].has_value())
                gunAudioSrc[i]->setPos(handPose[i]->pos);
            if (!trigger_old[i] && dat.trigger[i]) {
                gunAudioSrc[i]->play(gunSe.value());
                if (dat.handVib[i].has_value()) {
                    dat.handVib[i]->get().vibrate(1.0);
                }
            }
        }

        switch (scene)
        {
            case Scene::Title: {

                bool gameStart = false;

                bool gsSelected_old = gsSelected;
                gsSelected = false;
                for (int i = 0; i < 2; i++) {
                    bool gsSelectedHands_old = gsSelectedHands[i];
                    gsSelectedHands[i] = false;
                    if (handPose[i].has_value()) {
                        auto col = Rect{ gameStartStrPose, 2.5, 0.5 }.RayCast(handPose[i]->pos, handPose[i]->ori * glm::vec3{ 0, 0, -1 });
                        if (col.has_value()) {
                            rayD[i] = col.value().second;
                            gsSelectedHands[i] = true;
                            gsSelected = true;
                            if (gameDat.trigger[i])
                                gameStart = true;
                        }
                    }
                    if (gsSelectedHands_old != gsSelectedHands[i])
                        dat.handVib[i]->get().vibrate(0.1);
                }
                if (gsSelected != gsSelected_old) {

                }


                if (gameStart) {
                    scene = Scene::MainGame;
                    score = 0;
                    gameTimer = 30.0;
                }

                break;
            }
            case Scene::MainGame: {
                gameTimer -= dat.dt;

                for (int i = 0; i < 2; i++) {
                    if (!handPose[i].has_value())
                        continue;

                    std::optional<std::reference_wrapper<Target>> lockedon;
                    Target::Hit hit;

                    glm::vec3 colPos;
                    for (auto& target : targets) {
                        auto ray = handPose[i]->ori * glm::vec3{ 0, 0, -1 };
                        auto res = target.RayCast(handPose[i]->pos, ray);
                        if (!res.has_value())
                            continue;
                        auto d = res->rayLen;
                        if (!rayD[i].has_value() || d < rayD[i].value()) {
                            rayD[i] = d;
                            colPos = handPose[i]->pos + d * ray;

                            if (res->enable) {
                                lockedon.emplace(target);
                                hit = res.value();
                            }
                        }
                    }
                    if (lockedon.has_value() && gameDat.trigger[i]) {
                        lockedon->get().alive = false;

                        seManager->play(tgtSe.value(), hit.colPos);
                        for (int i = 0; i < 10; i++)
                            boomEffects.emplace_back(hit.colPos);

                        if (hit.dCenter < 0.04) {
                            scoreEffects.emplace_back(colPos, 0);
                            score += 100;
                        }
                        else if (hit.dCenter < 0.07) {
                            scoreEffects.emplace_back(colPos, 1);
                            score += 50;
                        }
                        else if (hit.dCenter < 0.13) {
                            scoreEffects.emplace_back(colPos, 2);
                            score += 30;
                        }
                        else {
                            scoreEffects.emplace_back(colPos, 3);
                            score += 10;
                        }
                    }
                }


                {
                    tgtTimer -= gameDat.dt;
                    if (tgtTimer <= 0) {
                        tgtTimer = 0.5;
                        targets.emplace_back();
                    }
                }

                for (auto& target : targets) {
                    target.proc(gameDat.dt);
                }
                {
                    auto remove_it = std::remove_if(targets.begin(), targets.end(), [&](const auto& target) {
                        return !target.alive;
                    });
                    targets.erase(remove_it, targets.end());
                }
                for (auto& effect : scoreEffects) {
                    effect.proc(gameDat.dt);
                }
                {
                    auto remove_it = std::remove_if(scoreEffects.begin(), scoreEffects.end(), [&](const auto& effect) {
                        return !effect.alive;
                    });
                    scoreEffects.erase(remove_it, scoreEffects.end());
                }
                for (auto& effect : boomEffects) {
                    effect.proc(gameDat.dt);
                }
                {
                    auto remove_it = std::remove_if(boomEffects.begin(), boomEffects.end(), [&](const auto& effect) {
                        return !effect.alive;
                    });
                    boomEffects.erase(remove_it, boomEffects.end());
                }

                if (gameTimer <= 0.0) {
                    scene = Scene::ScoreResult;
                    gameTimer = 8.0f;
                    seManager->play(buzzerSe.value(), sightBase.pos + fwdVec * 2.0f + upperVec * 2.0f);
                }

                for (int i = 1; i <= 5; i++) {
                    if (gameTimer >= i && gameTimer - dat.dt < i) {
                        seManager->play(cntSe.value(), sightBase.pos + fwdVec * 1.0f + upperVec * 1.0f);
                    }
                }
                break;
            }
            case Scene::ScoreResult: {
                gameTimer -= dat.dt;
                for (auto& target : targets) {
                    target.proc(gameDat.dt);
                }
                {
                    auto remove_it = std::remove_if(targets.begin(), targets.end(), [&](const auto& target) {
                        return !target.alive;
                    });
                    targets.erase(remove_it, targets.end());
                }
                for (auto& effect : scoreEffects) {
                    effect.proc(gameDat.dt);
                }
                {
                    auto remove_it = std::remove_if(scoreEffects.begin(), scoreEffects.end(), [&](const auto& effect) {
                        return !effect.alive;
                    });
                    scoreEffects.erase(remove_it, scoreEffects.end());
                }
                for (auto& effect : boomEffects) {
                    effect.proc(gameDat.dt);
                }
                {
                    auto remove_it = std::remove_if(boomEffects.begin(), boomEffects.end(), [&](const auto& effect) {
                        return !effect.alive;
                    });
                    boomEffects.erase(remove_it, boomEffects.end());
                }

                if (gameTimer >= 5.0f && gameTimer - dat.dt < 5.0f) {
                    seManager->play(gunSe.value(), sightBase.pos + fwdVec * 1.0f + upperVec * 1.0f);
                }

                if (gameTimer <= 0.0f) {
                    scene = Scene::Title;
                }

                break;
            }
            default:
                break;
        }
    }

    void proc(const GameData& dat) {
        for (int i = 0; i < 2; i++)
            trigger_old[i] = gameDat.trigger[i];
        gameDat = dat;


        if (initialized) {
            proc_main(dat);
            seManager->update(dat.dt);
        }
        else {
            proc_init(dat);
        }
    }

    void draw(IGraphicsProvider& g) {
        for (int i = 0; i < 2; i++) {
            if (handPose[i].has_value()) {
                g.DrawModel(gunModel, handPose[i]->pos, handPose[i]->ori, glm::vec3{ 0.05, 0.05, 0.05 });

                g.DrawModel(beamModel, handPose[i]->pos, handPose[i]->ori, glm::vec3{ 0.02, 0.02, rayD[i].has_value() ? rayD[i].value() : 10 });
                if(rayD[i].has_value())
                    g.DrawModel(sphereModel, handPose[i]->pos + float(rayD[i].value()) * (handPose[i]->ori * glm::vec3{ 0, 0, -1 }), handPose[i]->ori, glm::vec3{ 0.01, 0.01, 0.01 });
            }
        }

        g.DrawModel(testModel, stagePose.pos, stagePose.ori, glm::vec3(0.5, 0.02, 0.5),
                    glm::rotate(float(stageRotate * 2 * pi), glm::vec3(0, 1, 0)) * glm::rotate(float(pi), glm::vec3(0, 0, 1)));

        switch (scene)
        {
            case Scene::Title: {
                if (gsSelected)
                    g.DrawModel(gamestartSelectedModel, gameStartStrPose.pos, gameStartStrPose.ori, glm::vec3(0.5, 0.5, 0.5));
                else
                    g.DrawModel(gamestartModel, gameStartStrPose.pos, gameStartStrPose.ori, glm::vec3(0.5, 0.5, 0.5));
                break;
            }
            case Scene::MainGame: {
                for (const auto& target : targets) {
                    g.DrawModel(tgtModel, target.pose.pos, target.pose.ori, glm::vec3(0.2, 0.2, 0.2));
                }
                for (const auto& effect : scoreEffects) {
                    effect.draw(g);
                }
                for (const auto& effect : boomEffects) {
                    effect.draw(g);
                }

                g.DrawModel(timeModel, sightBase.pos + fwdVec * 20.0f + upperVec * 6.0f,
                            sightBase.ori * glm::rotate(glm::identity<glm::quat>(), float(pi), glm::vec3{ 0,1,0 }), glm::vec3(4.0, 4.0, 4.0));
                g.DrawModel(numberModel[int(gameTimer) / 10], sightBase.pos + fwdVec * 20.0f - rightVec * 3.0f,
                            sightBase.ori * glm::rotate(glm::identity<glm::quat>(), float(pi), glm::vec3{ 0,1,0 }), glm::vec3(10.0, 10.0, 10.0));
                g.DrawModel(numberModel[int(gameTimer) % 10], sightBase.pos + fwdVec * 20.0f + rightVec * 3.0f,
                            sightBase.ori * glm::rotate(glm::identity<glm::quat>(), float(pi), glm::vec3{ 0,1,0 }), glm::vec3(10.0, 10.0, 10.0));

                break;
            }
            case Scene::ScoreResult: {
                if (gameTimer < 7.0f) {
                    g.DrawModel(scoreStrModel, sightBase.pos + fwdVec * 20.0f + upperVec * 8.0f,
                                sightBase.ori * glm::rotate(glm::identity<glm::quat>(), float(pi), glm::vec3{ 0,1,0 }), glm::vec3(4.0, 4.0, 4.0));
                }

                if (gameTimer < 5.0f) {
                    int scoreDig = 0;
                    {
                        int tmp = score;
                        while (tmp > 0) {
                            scoreDig++;
                            tmp /= 10;
                        }
                        if (scoreDig == 0)
                            scoreDig = 1;
                    }
                    {
                        int tmp = score;
                        auto tmpOri = sightBase.ori * glm::rotate(glm::identity<glm::quat>(), float(pi), glm::vec3{ 0,1,0 });

                        for (int i = 0; i < scoreDig; i++) {
                            g.DrawModel(numberModel[int(tmp) % 10], sightBase.pos + fwdVec * 20.0f + rightVec * ((scoreDig - 1) / 2.0f - i) * 4.5f,
                                        tmpOri, glm::vec3(6.0, 6.0, 6.0));
                            tmp /= 10;
                        }
                    }
                }

                for (const auto& target : targets) {
                    g.DrawModel(tgtModel, target.pose.pos, target.pose.ori, glm::vec3(0.2, 0.2, 0.2));
                }
                for (const auto& effect : scoreEffects) {
                    effect.draw(g);
                }
                for (const auto& effect : boomEffects) {
                    effect.draw(g);
                }
            }
            default:
                break;
        }

    }

}
