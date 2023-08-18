#include <matchmaking/roundrobin.hpp>

#include <third_party/chess.hpp>

#include <logger.hpp>
#include <matchmaking/output/output_factory.hpp>
#include <pgn_builder.hpp>
#include <rand.hpp>

namespace fast_chess {

RoundRobin::RoundRobin(const cmd::TournamentOptions& game_config)
    : output_(getNewOutput(game_config.output)), game_config_(game_config) {
    auto filename = (game_config.pgn.file.empty() ? "fast-chess" : game_config.pgn.file);

    if (game_config.output == OutputType::FASTCHESS) {
        filename += ".pgn";
    }

    file_writer_.open(filename);

    setupEpdOpeningBook();
    setupPgnOpeningBook();

    // Resize the thread pool
    pool_.resize(game_config_.concurrency);

    // Initialize the SPRT test
    sprt_ = SPRT(game_config_.sprt.alpha, game_config_.sprt.beta, game_config_.sprt.elo0,
                 game_config_.sprt.elo1);
}

void RoundRobin::setupEpdOpeningBook() {
    // Set the seed for the random number generator
    Random::mersenne_rand.seed(game_config_.seed);

    if (game_config_.opening.file.empty() || game_config_.opening.format != FormatType::EPD) {
        return;
    }

    // Read the opening book from file
    std::ifstream openingFile;
    std::string line;
    openingFile.open(game_config_.opening.file);

    while (std::getline(openingFile, line)) {
        opening_book_epd_.emplace_back(line);
    }

    openingFile.close();

    if (opening_book_epd_.empty()) {
        throw std::runtime_error("No openings found in EPD file: " + game_config_.opening.file);
    }

    if (game_config_.opening.order == OrderType::RANDOM) {
        // Fisher-Yates / Knuth shuffle
        for (std::size_t i = 0; i <= opening_book_epd_.size() - 2; i++) {
            std::size_t j = i + (Random::mersenne_rand() % (opening_book_epd_.size() - i));
            std::swap(opening_book_epd_[i], opening_book_epd_[j]);
        }
    }
}

void RoundRobin::setupPgnOpeningBook() {
    // Read the opening book from file
    if (game_config_.opening.file.empty() || game_config_.opening.format != FormatType::PGN) {
        return;
    }

    const PgnReader pgn_reader = PgnReader(game_config_.opening.file);
    opening_book_pgn_ = pgn_reader.getPgns();

    if (opening_book_pgn_.empty()) {
        throw std::runtime_error("No openings found in PGN file: " + game_config_.opening.file);
    }

    if (game_config_.opening.order == OrderType::RANDOM) {
        // Fisher-Yates / Knuth shuffle
        for (std::size_t i = 0; i <= opening_book_pgn_.size() - 2; i++) {
            std::size_t j = i + (Random::mersenne_rand() % (opening_book_pgn_.size() - i));
            std::swap(opening_book_pgn_[i], opening_book_pgn_[j]);
        }
    }
}

void RoundRobin::start(const std::vector<EngineConfiguration>& engine_configs) {
    Logger::debug("Starting round robin tournament...");

    create(engine_configs);

    // Wait for games to finish
    while (match_count_ < total_ && !atomic::stop) {
    }
}

void RoundRobin::create(const std::vector<EngineConfiguration>& engine_configs) {
    total_ = (engine_configs.size() * (engine_configs.size() - 1) / 2) * game_config_.rounds *
             game_config_.games;

    for (std::size_t i = 0; i < engine_configs.size(); i++) {
        for (std::size_t j = i + 1; j < engine_configs.size(); j++) {
            for (int k = 0; k < game_config_.rounds; k++) {
                pool_.enqueue(&RoundRobin::createPairings, this, engine_configs[i],
                              engine_configs[j], k);
            }
        }
    }
}

void RoundRobin::updateSprtStatus(const std::vector<EngineConfiguration>& engine_configs) {
    const Stats stats = result_.getStats(engine_configs[0].name, engine_configs[1].name);
    const double llr = sprt_.getLLR(stats.wins, stats.draws, stats.losses);

    if (sprt_.getResult(llr) != SPRT_CONTINUE || match_count_ == total_) {
        atomic::stop = true;

        Logger::cout("SPRT test finished: " + sprt_.getBounds() + " " + sprt_.getElo());
        output_->printElo(stats, engine_configs[0].name, engine_configs[1].name, match_count_);
        output_->endTournament();

        stop();
    }
}

void RoundRobin::createPairings(const EngineConfiguration& player1,
                                const EngineConfiguration& player2, int current) {
    std::pair<EngineConfiguration, EngineConfiguration> configs = {player1, player2};

    // Swap the players randomly when using Cutechess
    if (Random::boolean() && game_config_.output == OutputType::CUTECHESS) {
        std::swap(configs.first, configs.second);
    }

    const auto opening = fetchNextOpening();

    Stats stats;
    for (int i = 0; i < game_config_.games; i++) {
        const auto idx = current * game_config_.games + (i + 1);

        output_->startGame(configs.first.name, configs.second.name, idx, game_config_.rounds * 2);
        const auto [success, result, reason] = playGame(configs, opening, idx);
        output_->endGame(result, configs.first.name, configs.second.name, reason, idx);

        if (atomic::stop) return;

        // If the game failed to start, try again
        if (!success && game_config_.recover) {
            i--;
            continue;
        }

        // Invert the result of the other player, so that stats are always from the perspective of
        // the first player.
        if (player1.name != configs.first.name) {
            stats += ~result;
        } else {
            stats += result;
        }

        match_count_++;

        if (!game_config_.report_penta) {
            result_.updateStats(configs.first.name, configs.second.name, result);
            output_->printInterval(sprt_, result_.getStats(player1.name, player2.name),
                                   player1.name, player2.name, match_count_);
        }

        std::swap(configs.first, configs.second);
    }

    // track penta stats
    if (game_config_.report_penta) {
        stats.penta_WW += stats.wins == 2;
        stats.penta_WD += stats.wins == 1 && stats.draws == 1;
        stats.penta_WL += stats.wins == 1 && stats.losses == 1;
        stats.penta_DD += stats.draws == 2;
        stats.penta_LD += stats.losses == 1 && stats.draws == 1;
        stats.penta_LL += stats.losses == 2;

        result_.updateStats(configs.first.name, configs.second.name, stats);
        output_->printInterval(sprt_, result_.getStats(player1.name, player2.name), player1.name,
                               player2.name, match_count_);
    }

    if (sprt_.isValid()) {
        updateSprtStatus({player1, player2});
    }
}

std::tuple<bool, Stats, std::string> RoundRobin::playGame(
    const std::pair<EngineConfiguration, EngineConfiguration>& configs, const Opening& opening,
    int round_id) {
    auto match = Match(game_config_, opening, round_id);

    try {
        match.start(configs.first, configs.second);
    } catch (const std::exception& e) {
        Logger::error(e.what(), std::this_thread::get_id(), "fast-chess::RoundRobin::playGame");
        return {false, Stats(), "exception"};
    }

    const auto match_data = match.get();

    const auto pgn_builder = PgnBuilder(match_data, game_config_);

    // If the game was stopped, don't write the PGN
    if (match_data.termination != MatchTermination::INTERRUPT) {
        file_writer_.write(pgn_builder.get());
    }

    return {true, updateStats(match_data), match_data.reason};
}

Stats RoundRobin::updateStats(const MatchData& match_data) {
    Stats stats;

    if (match_data.players.first.result == chess::GameResult::WIN) {
        stats.wins++;
    } else if (match_data.players.first.result == chess::GameResult::LOSE) {
        stats.losses++;
    } else {
        stats.draws++;
    }

    return stats;
}

Opening RoundRobin::fetchNextOpening() {
    static uint64_t opening_index = 0;

    if (game_config_.opening.format == FormatType::PGN) {
        return opening_book_pgn_[(game_config_.opening.start + opening_index++) %
                                 opening_book_pgn_.size()];
    } else if (game_config_.opening.format == FormatType::EPD) {
        Opening opening;

        opening.fen = opening_book_epd_[(game_config_.opening.start + opening_index++) %
                                        opening_book_epd_.size()];

        return opening;
    }

    Logger::cout("Unknown opening format: " + int(game_config_.opening.format));

    return {chess::STARTPOS, {}};
}

}  // namespace fast_chess