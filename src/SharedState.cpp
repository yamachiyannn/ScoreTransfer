#include "ScoreTransfer.hpp"

namespace ScoreTransfer {
std::atomic<int> g_sceneId{-1};
std::atomic<int> g_leftWins{0};
std::atomic<int> g_rightWins{0};
std::atomic<bool> g_stopRequested{false};
HMODULE g_module = nullptr;

bool IsDisplayScene(int sceneId)
{
    return sceneId == 8 || sceneId == 9 || sceneId == 10 ||
           sceneId == 11 || sceneId == 13 || sceneId == 14;
}
}
