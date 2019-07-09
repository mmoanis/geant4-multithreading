#include <string>
#include <iostream>

#include "simulation/geometry.hpp"
#include "simulation/generator.hpp"

#include <G4RunManager.hh>
#include <G4StepLimiterPhysics.hh>
#include <G4PhysListFactory.hh>
#include <G4UImanager.hh>

#include <G4RNGHelper.hh>

class RunManager : public G4RunManager {
	public:
		RunManager() : G4RunManager() {
			rng_engine_ = G4Random::getTheEngine();
			seed_array_ = new double[1024*2];
			
		};

		virtual ~RunManager() {};

		virtual void InitializeEventLoop(G4int n_event, const char *macro_file, G4int n_select) override {
			if (!fakeRun) {

                // create a new engine with the same type as the default engine
				if (rng_engine2_ == nullptr) {
					if ( dynamic_cast<const CLHEP::HepJamesRandom*>(rng_engine_) ) {
				       rng_engine2_ = new CLHEP::HepJamesRandom;
				    }
				    if ( dynamic_cast<const CLHEP::MixMaxRng*>(rng_engine_) ) {
				       rng_engine2_ = new CLHEP::MixMaxRng;
				    }
				    if ( dynamic_cast<const CLHEP::RanecuEngine*>(rng_engine_) ) {
				       rng_engine2_ = new CLHEP::RanecuEngine;
				    }
				    if ( dynamic_cast<const CLHEP::Ranlux64Engine*>(rng_engine_) ) {
				       rng_engine2_ = new CLHEP::Ranlux64Engine;
				    }
				    if ( dynamic_cast<const CLHEP::MTwistEngine*>(rng_engine_) ) {
				       rng_engine2_ = new CLHEP::MTwistEngine;
				    }
				    if ( dynamic_cast<const CLHEP::DualRand*>(rng_engine_) ) {
				       rng_engine2_ = new CLHEP::DualRand;
				    }
				    if ( dynamic_cast<const CLHEP::RanluxEngine*>(rng_engine_) ) {
				       rng_engine2_ = new CLHEP::RanluxEngine;
				    }
				    if ( dynamic_cast<const CLHEP::RanshiEngine*>(rng_engine_) ) {
				       rng_engine2_ = new CLHEP::RanshiEngine;
				    }


				   if (rng_engine2_ != nullptr) {
                        // Replace the RNG engine with the new one
					   G4Random::setTheEngine(rng_engine2_);
				   }
				}

                // For each call to BeamOn, we draw random numbers from the first engine
                // and use them as seeds to the second one. This is exactly what the MTRunManager
                // does where the second engine is a thread local version
				G4RNGHelper *helper = G4RNGHelper::GetInstance();
				rng_engine_->flatArray(2*n_event, &seed_array_[0]);
				helper->Fill(&seed_array_[0], n_event, n_event, 2);

				long s1 = helper->GetSeed(0);
				long s2 = helper->GetSeed(1);
				std::cout << "S1=" << s1 << " S2=" << s2 << std::endl;
				long seeds[3] = { s1, s2, 0 };
				G4Random::setTheSeeds(seeds, -1);
			}

			G4RunManager::InitializeEventLoop(n_event, macro_file, n_select);

		};

	private:
		// Two RNG engines, the first one is used to generate seeds for the second one
		// while the second one is used by the simulation flow
		CLHEP::HepRandomEngine* rng_engine_{nullptr}, *rng_engine2_{nullptr};	
		double *seed_array_{nullptr};
};


int main() {
    // Create the G4 run manager
    std::unique_ptr<RunManager> run_manager_g4_ = std::make_unique<RunManager>();

    // Initialize the geometry:
    auto geometry_construction = new GeometryConstructionG4();
    run_manager_g4_->SetUserInitialization(geometry_construction);
    run_manager_g4_->InitializeGeometry();

    // Initialize physics
    G4PhysListFactory physListFactory;
    G4VModularPhysicsList* physicsList = physListFactory.GetReferencePhysList("FTFP_BERT_EMZ");
    physicsList->RegisterPhysics(new G4StepLimiterPhysics());
    run_manager_g4_->SetUserInitialization(physicsList);
    run_manager_g4_->InitializePhysics();

    // Particle source
    run_manager_g4_->SetUserInitialization(new GeneratorActionInitialization());

    G4UImanager* ui_g4 = G4UImanager::GetUIpointer();
    #define G4_NUM_SEEDS 10
    std::string seed_command = "/random/setSeeds ";
    for(int i = 0; i < G4_NUM_SEEDS; ++i) {
        seed_command += std::to_string(i);
        if(i != G4_NUM_SEEDS - 1) {
            seed_command += " ";
        }
    }
    ui_g4->ApplyCommand(seed_command);

    // Initialize the full run manager to ensure correct state flags
    run_manager_g4_->Initialize();

    // Run our own event loop:
    for(int i = 0; i < 100000; i++) {
        run_manager_g4_->BeamOn(1);
    }


    return 0;
}
