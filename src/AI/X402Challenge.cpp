#include "X402Challenge.hpp"

#include <nlohmann/json.hpp>

namespace AI {

std::optional<X402Challenge> X402Challenge::parse(const std::string& body) {
    auto j = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;

    // The challenge lives under "x402" on a 402; /warm/price returns it flat.
    const nlohmann::json& x = (j.contains("x402") && j["x402"].is_object()) ? j["x402"] : j;

    X402Challenge c;
    c.asset   = x.value("asset", "");
    c.issuer  = x.value("issuer", "");
    c.amount  = x.value("amount", "");
    c.payTo   = x.value("pay_to", "");
    c.network = x.value("network", "");
    c.memo    = x.value("memo", "");
    c.priceId = x.value("price_id", "");
    c.horizon = x.value("horizon", "");
    // window_s may arrive as a number or a numeric string.
    if (x.contains("window_s")) {
        const auto& w = x["window_s"];
        if (w.is_number()) c.windowS = w.get<double>();
        else if (w.is_string()) { try { c.windowS = std::stod(w.get<std::string>()); } catch (...) {} }
    }
    return c;
}

}  // namespace AI
