#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

// Input Buffer: look-ahead click timing
// -------------------------------------
// When you click, the mod saves the level state and quietly simulates the
// next moments with your click placed 0, 1, 2 ... N physics steps later.
// It picks the earliest placement where you survive the look-ahead window,
// rewinds, and then plays your click on that step for real. If no placement
// survives, the click happens right away, exactly like vanilla.

constexpr int kJump = 1;

// ---------- settings ----------

static bool modEnabled() {
	return Mod::get()->getSettingValue<bool>("enabled");
}

static int maxDelaySteps() {
	auto frames = Mod::get()->getSettingValue<int64_t>("extra-frames");
	auto unit = Mod::get()->getSettingValue<std::string>("frame-unit");
	int stepsPerFrame = unit.rfind("60", 0) == 0 ? 4 : 1; // 60 FPS frame = 4 physics steps
	return static_cast<int>(frames) * stepsPerFrame;
}

// Most simulated physics steps allowed per click (about 1.25 s of game time).
constexpr int kStepBudget = 300;

static int lookAheadSteps() {
	return static_cast<int>(Mod::get()->getSettingValue<int64_t>("look-ahead"));
}

// Hold modes keep the button down; everything else is treated as a tap.
static bool isHoldMode(PlayerObject* p) {
	return p && (p->m_isShip || p->m_isDart || p->m_isSwing);
}

// ---------- simulation state shared with the death hook ----------

static bool g_simulating = false;
static bool g_simDied = false;

struct StepArgs {
	float dt;
	bool half;
	bool last;
};

class $modify(BufferedLayer, GJBaseGameLayer) {
	struct Scheduled {
		bool active = false;          // a click is waiting for its chosen step
		int stepsLeft = 0;            // steps until we press for real
		bool isPlayer1 = true;
		bool releaseRequested = false;// the real button came up before we pressed
	};

	struct Fields {
		Scheduled sched[2];
		bool releaseNextStep[2] = { false, false };
		bool releaseIsPlayer1[2] = { true, true };
		bool decisionPending[2] = { false, false }; // press arrived mid-step
		bool decisionIsPlayer1[2] = { true, true };
		bool insideStep = false;
		std::vector<StepArgs> framePattern;   // step args of the last full frame
		std::vector<StepArgs> currentFrame;
		size_t stepInFrame = 0;
	};

	PlayLayer* asPlayLayer() {
		auto pl = PlayLayer::get();
		return (pl && static_cast<GJBaseGameLayer*>(pl) == this) ? pl : nullptr;
	}

	bool isTwoPlayer() {
		return m_levelSettings && m_levelSettings->m_twoPlayerMode;
	}

	int slotFor(bool isPlayer1) {
		return (isTwoPlayer() && !isPlayer1) ? 1 : 0;
	}

	PlayerObject* mainPlayerFor(int slot) {
		return (isTwoPlayer() && slot == 1) ? m_player2 : m_player1;
	}

	StepArgs simArgs(size_t k) {
		auto& pat = m_fields->framePattern;
		if (pat.empty()) return { 1.f / 240.f, false, true };
		return pat[(m_fields->stepInFrame + k) % pat.size()];
	}

	// Run the vanilla step, bypassing our own processCommands logic.
	// Simulated steps never count as the last step of a frame, which skips
	// the visual-only work the game does once per rendered frame.
	void rawStep(StepArgs const& a) {
		GJBaseGameLayer::processCommands(a.dt, a.half, false);
	}

	// Simulate placing the press `delay` steps from now. True if the player survives.
	// `budget` is the number of simulated steps left for this click; it is reduced as we go.
	bool survivesWithDelay(int delay, bool isPlayer1, bool tap, int horizon, int& budget) {
		g_simDied = false;
		g_simulating = true;
		for (int k = 0; k < horizon; ++k) {
			if (k == delay) GJBaseGameLayer::handleButton(true, kJump, isPlayer1);
			if (tap && k == delay + 1) GJBaseGameLayer::handleButton(false, kJump, isPlayer1);
			rawStep(simArgs(k));
			--budget;
			if (g_simDied) break;
		}
		g_simulating = false;
		return !g_simDied;
	}

	// Decide on which step (0..max) the press should land. Restores all state.
	int chooseDelay(bool isPlayer1) {
		auto pl = asPlayLayer();
		if (!pl || m_playerDied) return 0;

		int maxDelay = maxDelaySteps();
		int horizon = lookAheadSteps();
		bool tap = !isHoldMode(mainPlayerFor(slotFor(isPlayer1)));

		auto checkpoint = pl->createCheckpoint();
		if (!checkpoint) return 0;
		checkpoint->retain();

		auto savedQueue = m_queuedButtons;
		bool savedDied = m_playerDied;

		// Hard cap on work per click so a click never freezes the game.
		// Rewinding costs about as much as a few dozen steps, so it's charged too.
		constexpr int kRewindCost = 20;
		int budget = kStepBudget;

		int chosen = 0;
		bool found = false;
		for (int d = 0; d <= maxDelay; ++d) {
			if (budget < horizon + d) break; // not enough left to test this one properly
			if (d > 0) {
				pl->loadFromCheckpoint(checkpoint);
				budget -= kRewindCost;
			}
			m_queuedButtons.clear();
			if (survivesWithDelay(d, isPlayer1, tap, horizon + d, budget)) {
				chosen = d;
				found = true;
				break;
			}
		}

		pl->loadFromCheckpoint(checkpoint);
		m_queuedButtons = savedQueue;
		m_playerDied = savedDied;
		checkpoint->release();

		return found ? chosen : 0;
	}

	void schedulePress(bool isPlayer1) {
		int slot = slotFor(isPlayer1);
		auto& s = m_fields->sched[slot];
		s.active = true;
		s.isPlayer1 = isPlayer1;
		s.releaseRequested = false;
		s.stepsLeft = chooseDelay(isPlayer1);
	}

	void handleButton(bool down, int button, bool isPlayer1) {
		if (g_simulating || !modEnabled() || button != kJump || !asPlayLayer()) {
			GJBaseGameLayer::handleButton(down, button, isPlayer1);
			return;
		}

		int slot = slotFor(isPlayer1);
		auto& s = m_fields->sched[slot];

		if (!down) {
			if (s.active) {
				s.releaseRequested = true; // release right after our press lands
				return;
			}
			if (m_fields->decisionPending[slot]) {
				// Tap inside a single step: remember the release for after the decision.
				s.releaseRequested = true;
				return;
			}
			GJBaseGameLayer::handleButton(false, button, isPlayer1);
			return;
		}

		// New press while an old one is still waiting: fire the old one now.
		if (s.active) {
			s.active = false;
			GJBaseGameLayer::handleButton(true, kJump, s.isPlayer1);
			GJBaseGameLayer::handleButton(false, kJump, s.isPlayer1);
		}

		if (m_fields->insideStep) {
			// We can't rewind in the middle of a step; decide right after it ends.
			m_fields->decisionPending[slot] = true;
			m_fields->decisionIsPlayer1[slot] = isPlayer1;
			s.releaseRequested = false;
			return;
		}

		schedulePress(isPlayer1);
		fireIfDue(slot);
	}

	void fireIfDue(int slot) {
		auto& s = m_fields->sched[slot];
		if (!s.active || s.stepsLeft > 0) return;
		s.active = false;
		GJBaseGameLayer::handleButton(true, kJump, s.isPlayer1);
		if (s.releaseRequested) {
			m_fields->releaseNextStep[slot] = true;
			m_fields->releaseIsPlayer1[slot] = s.isPlayer1;
		}
	}

	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		if (g_simulating || !modEnabled() || !asPlayLayer()) {
			GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
			return;
		}

		auto& f = m_fields;

		// Remember how the game steps so simulations step the same way.
		f->currentFrame.push_back({ dt, isHalfTick, isLastTick });
		if (isLastTick) {
			f->framePattern = f->currentFrame;
			f->currentFrame.clear();
		}

		for (int slot = 0; slot < 2; ++slot) {
			if (f->releaseNextStep[slot]) {
				f->releaseNextStep[slot] = false;
				GJBaseGameLayer::handleButton(false, kJump, f->releaseIsPlayer1[slot]);
			}
			fireIfDue(slot);
		}

		f->insideStep = true;
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
		f->insideStep = false;

		f->stepInFrame = isLastTick ? 0 : f->stepInFrame + 1;

		for (int slot = 0; slot < 2; ++slot) {
			auto& s = f->sched[slot];
			if (f->decisionPending[slot]) {
				// The press came in during this step; delay 0 means the start of the next one.
				f->decisionPending[slot] = false;
				bool wantsRelease = s.releaseRequested;
				schedulePress(f->decisionIsPlayer1[slot]);
				s.releaseRequested = wantsRelease;
				continue;
			}
			if (s.active && s.stepsLeft > 0) s.stepsLeft--;
		}
	}

	void clearBuffers() {
		auto& f = m_fields;
		for (int slot = 0; slot < 2; ++slot) {
			f->sched[slot] = {};
			f->releaseNextStep[slot] = false;
			f->decisionPending[slot] = false;
		}
	}
};

class $modify(BufferedPlayLayer, PlayLayer) {
	// Deaths inside our test runs must be caught before any other mod
	// (Mega Hack etc.) sees them, or a simulated death becomes a real one.
	static void onModify(auto& self) {
		(void)self.setHookPriority("PlayLayer::destroyPlayer", Priority::FirstPre);
		(void)self.setHookPriority("PlayLayer::levelComplete", Priority::FirstPre);
	}

	bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
		if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
		if (modEnabled() && Mod::get()->getSettingValue<bool>("safe-mode")) {
			m_isTestMode = true;
		}
		return true;
	}

	void destroyPlayer(PlayerObject* player, GameObject* object) {
		if (g_simulating) {
			// The hidden anti-cheat spike isn't a real death.
			if (object != m_anticheatSpike) g_simDied = true;
			return;
		}
		PlayLayer::destroyPlayer(player, object);
	}

	void levelComplete() {
		if (g_simulating) return;
		PlayLayer::levelComplete();
	}

	void resetLevel() {
		static_cast<BufferedLayer*>(static_cast<GJBaseGameLayer*>(this))->clearBuffers();
		PlayLayer::resetLevel();
	}
};
