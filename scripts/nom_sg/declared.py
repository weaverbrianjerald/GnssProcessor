"""The closed list of declared substitution classes of card NOM3 step SG (design section 3.3).

Every generated line, every substitution inside a frozen piece and every frozen range that no
new body carries names one of these classes; the verifier (scripts/nom_sg_verify.py) fails any
other.  The value is the proof obligation, checked where the verifier states it.
"""

CLASSES = {
    # The orchestrator keeps EP1's signature (and its closing brace).
    "SIGNATURE": "the orchestrator's signature lines equal the frozen ones",
    # A stage or helper opens with bindings (one line per name the body reads).
    "PREAMBLE": "each line is a frozen binding line verbatim, a REALIZE / KNOB line of the "
                "table, the frozen emitEvent lambda, or FRESH_STATELESS; S3 checks the set",
    # A crossing local's declaration becomes its typed value's member.
    "REALIZE": "the member's type equals the local's; T-B5 default equals the frozen "
               "initializer; T-B7 stores the frozen initializer at the frozen position",
    # An inner block becomes a helper's body; one statement takes its place.
    "EXTRACT": "body verbatim (S2); the helper's parameters bind the names the block read "
               "(S3); the call names the helper that carries the range",
    # The bookkeeping of an exit becomes `return EpochAction::SkipEpoch;`.
    "EXIT_RETURN": "the removed statements are the exit's kSolveSkips row (S6)",
    # The gate's exit test becomes its generated tail.
    "EXIT_TEST": "nothing between the gate's tail and the frozen test is code (R21 is the exit)",
    # The five frozen exit tails are kSolveSkips and networkSolveSkip.
    "EXIT_TABLE": "per exit, the facts of the frozen tail equal the row's (S6)",
    # `++n_sigma0_withheld` now precedes `++n_skipped`.
    "COUNTER_ORDER": "no statement between the two positions reads either counter",
    # GateWithheld's H sigma0 0.0 equals the frozen unassigned default.
    "VALUE_EQUAL": "NetworkProducerStatus::sigma0{0.0}; SolveSkipFacts::of(datum) leaves 0.0",
    # A write or read of result.X names its realization.
    "RESULT_FIELD": "the realization table; the epilogue runs after every stage",
    # Three member moves into result.handoff become one struct move.
    "MERGE": "NetworkPostFitHandoff has exactly those three members",
    # `const T x = E;` as a stage's only output becomes `return E;`.
    "RESULT_RETURN": "the range holds no other statement that reads x",
    # retro_feed becomes NetworkKnobs::retro_feed.
    "KNOB": "the knob's initializer is the frozen text with each alias replaced by its field",
    # `NetworkSbasis sb;` is declared in each stage that calls it.
    "FRESH_STATELESS": "NetworkSbasis has no non-static data member (S12)",
    # The frozen emitEvent lambda repeated in a stage preamble.
    "LAMBDA_TEXT": "text identical to the frozen lambda",
    # emitEpochStatus's body is networkSolveEpochStatusRow's.
    "LAMBDA_TO_FN": "body verbatim; the captures bind through the preamble",
    # Dead code removed.
    "DROP": "no reader in the frozen function",
    # A declaration moves to where it is first needed.
    "DECL_MOVE": "no statement between reads it",
    # Comment-only lines move to a banner or the orchestrator.
    "COMMENT_MOVE": "no code line in the range",
    # A generated stage call in the orchestrator, with its exit test.
    "STAGE_CALL": "the calls, in order, are the banner table's (S5, L-SG)",
    # The generated tail of a stage.
    "TAIL": "return EpochAction::Proceed; or the typed return",
    # SG2: a block's locals read later become const bindings to a pure function's result.
    "REBIND": "one binding per name read after the block; no later write",
    # SG2: a catch clause's body becomes a helper's body.
    "CATCH_BODY": "EXTRACT's obligations; the catch header, the call and any jump stay",
    # SG2: a type declared with a local moves to a header type (enum class Kind -> WeChoice::Kind).
    "TYPE_MOVE": "the enumerators are the same, in the same order; `using Kind = WeChoice::Kind;` binds the name",
    # SG2: a function-local constant becomes a file-scope constexpr with the same name and value.
    "HOIST": "same name, type and value at file scope; the local line is removed",
    # SG2: a local shared by the lambdas becomes a WeEpochState member (as REALIZE).
    "STATE": "member type equals the local's; its default or the stored initializer equals the frozen one",
    # SG2: a captured lambda becomes a free function behind a one-line forwarding lambda.
    "CALLABLE": "every call site character-exact; the forwarding lambda forwards each argument",
}
