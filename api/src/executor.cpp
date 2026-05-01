#include <executor.hpp>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif

#include <thrift/transport/TBufferTransports.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "serializer.hpp"

#include <csnode/configholder.hpp>

#include <solver/solvercore.hpp>
#include <solver/smartcontracts.hpp>

#ifdef USE_EMBEDDED_JVM
#include <cstdlib>
#include <thrift/transport/TBufferTransports.h>
#include <thrift/protocol/TBinaryProtocol.h>
#include <jvm_embed/jvm_embed.hpp>

namespace {
// Translate a TaggedVariant back into a Thrift Variant. Returns false only
// for VTAG_OTHER (toString-only fallback) — every other case round-trips.
bool taggedToThriftVariant(const jvm_embed::Vm::Variant& src, ::general::Variant& out) {
    switch (src.tag) {
        case jvm_embed::Vm::VTAG_BOOL:    out.__set_v_boolean(src.boolVal); return true;
        case jvm_embed::Vm::VTAG_LONG:    out.__set_v_long(src.longVal); return true;
        case jvm_embed::Vm::VTAG_DOUBLE:  out.__set_v_double(src.doubleVal); return true;
        case jvm_embed::Vm::VTAG_STRING:  out.__set_v_string(src.stringVal); return true;
        case jvm_embed::Vm::VTAG_BYTES:   out.__set_v_byte_array(std::string(src.bytesVal.begin(), src.bytesVal.end())); return true;
        case jvm_embed::Vm::VTAG_VOID:    out.__set_v_void(0); return true;
        case jvm_embed::Vm::VTAG_NULL:    out.__set_v_null(""); return true;
        case jvm_embed::Vm::VTAG_AMOUNT: {
            ::general::Amount amt;
            amt.integral = static_cast<int32_t>(src.longVal);
            int64_t frac = 0;
            if (src.bytesVal.size() == 8) {
                for (size_t i = 0; i < 8; ++i) frac = (frac << 8) | int64_t(src.bytesVal[i]);
            }
            amt.fraction = frac;
            out.__set_v_amount(amt);
            return true;
        }
        case jvm_embed::Vm::VTAG_THRIFT_BINARY: {
            // Recursive Variant cases (V_LIST/V_MAP/V_SET/V_ARRAY/V_OBJECT)
            // are transported as a Thrift binary blob; deserialize back into
            // a real Variant on this side.
            if (src.bytesVal.empty()) return true;
            try {
                auto buf = ::apache::thrift::stdcxx::shared_ptr<::apache::thrift::transport::TMemoryBuffer>(
                    new ::apache::thrift::transport::TMemoryBuffer(
                        const_cast<uint8_t*>(src.bytesVal.data()),
                        static_cast<uint32_t>(src.bytesVal.size())));
                ::apache::thrift::stdcxx::shared_ptr<::apache::thrift::protocol::TProtocol> proto(
                    new ::apache::thrift::protocol::TBinaryProtocol(buf));
                out.read(proto.get());
                return true;
            } catch (...) {
                return false;
            }
        }
        case jvm_embed::Vm::VTAG_OTHER:
        default:
            return false;
    }
}

// Reverse direction: pull tag + value out of a Thrift Variant. Returns false
// if the Variant uses a structural case the bridge can't reconstruct.
bool thriftVariantToTagged(const ::general::Variant& v,
                           int32_t& tag, bool& b, int64_t& l, double& d,
                           std::string& s, std::vector<uint8_t>& by) {
    if      (v.__isset.v_boolean)     { tag = jvm_embed::Vm::VTAG_BOOL;   b = v.v_boolean; }
    else if (v.__isset.v_boolean_box) { tag = jvm_embed::Vm::VTAG_BOOL;   b = v.v_boolean_box; }
    else if (v.__isset.v_byte)        { tag = jvm_embed::Vm::VTAG_LONG;   l = v.v_byte; }
    else if (v.__isset.v_byte_box)    { tag = jvm_embed::Vm::VTAG_LONG;   l = v.v_byte_box; }
    else if (v.__isset.v_short)       { tag = jvm_embed::Vm::VTAG_LONG;   l = v.v_short; }
    else if (v.__isset.v_short_box)   { tag = jvm_embed::Vm::VTAG_LONG;   l = v.v_short_box; }
    else if (v.__isset.v_int)         { tag = jvm_embed::Vm::VTAG_LONG;   l = v.v_int; }
    else if (v.__isset.v_int_box)     { tag = jvm_embed::Vm::VTAG_LONG;   l = v.v_int_box; }
    else if (v.__isset.v_long)        { tag = jvm_embed::Vm::VTAG_LONG;   l = v.v_long; }
    else if (v.__isset.v_long_box)    { tag = jvm_embed::Vm::VTAG_LONG;   l = v.v_long_box; }
    else if (v.__isset.v_float)       { tag = jvm_embed::Vm::VTAG_DOUBLE; d = v.v_float; }
    else if (v.__isset.v_float_box)   { tag = jvm_embed::Vm::VTAG_DOUBLE; d = v.v_float_box; }
    else if (v.__isset.v_double)      { tag = jvm_embed::Vm::VTAG_DOUBLE; d = v.v_double; }
    else if (v.__isset.v_double_box)  { tag = jvm_embed::Vm::VTAG_DOUBLE; d = v.v_double_box; }
    else if (v.__isset.v_string)      { tag = jvm_embed::Vm::VTAG_STRING; s = v.v_string; }
    else if (v.__isset.v_big_decimal) { tag = jvm_embed::Vm::VTAG_STRING; s = v.v_big_decimal; }
    else if (v.__isset.v_byte_array)  { tag = jvm_embed::Vm::VTAG_BYTES;  by.assign(v.v_byte_array.begin(), v.v_byte_array.end()); }
    else if (v.__isset.v_null)        { tag = jvm_embed::Vm::VTAG_NULL; }
    else if (v.__isset.v_void)        { tag = jvm_embed::Vm::VTAG_VOID; }
    else if (v.__isset.v_amount)      {
        tag = jvm_embed::Vm::VTAG_AMOUNT;
        l = v.v_amount.integral;
        int64_t frac = v.v_amount.fraction;
        by.resize(8);
        for (int i = 7; i >= 0; --i) { by[i] = static_cast<uint8_t>(frac & 0xFF); frac >>= 8; }
    }
    else {
        // Recursive / structural cases (V_LIST/V_MAP/V_SET/V_ARRAY/V_OBJECT).
        // Serialize the whole Variant via Thrift binary and ship as bytes;
        // bridge deserializes back into a real Variant on the other side.
        try {
            auto buf = ::apache::thrift::stdcxx::shared_ptr<::apache::thrift::transport::TMemoryBuffer>(
                new ::apache::thrift::transport::TMemoryBuffer());
            ::apache::thrift::stdcxx::shared_ptr<::apache::thrift::protocol::TProtocol> proto(
                new ::apache::thrift::protocol::TBinaryProtocol(buf));
            const_cast<::general::Variant&>(v).write(proto.get());
            uint8_t* ptr = nullptr; uint32_t sz = 0;
            buf->getBuffer(&ptr, &sz);
            tag = jvm_embed::Vm::VTAG_THRIFT_BINARY;
            by.assign(ptr, ptr + sz);
        } catch (...) {
            return false;
        }
    }
    return true;
}
}  // namespace
#endif

void cs::ExecutorSettings::set(cs::Reference<const BlockChain> blockchain, cs::Reference<const cs::SolverCore> solver) {
    blockchain_ = blockchain;
    solver_ = solver;
}

cs::ExecutorSettings::Types cs::ExecutorSettings::get() {
    auto tuple = std::make_tuple(std::any_cast<cs::Reference<const BlockChain>>(blockchain_),
                                 std::any_cast<cs::Reference<const cs::SolverCore>>(solver_));

    blockchain_.reset();
    solver_.reset();

    return tuple;
}

void cs::Executor::executeByteCode(executor::ExecuteByteCodeResult& resp, const std::string& address, const std::string& smart_address,
                                   const std::vector<general::ByteCodeObject>& code, const std::string& state,
                                   std::vector<executor::MethodHeader>& methodHeader, bool isGetter, cs::Sequence sequence) {
    static std::mutex mutex;
    std::lock_guard lock(mutex);  // temporary solution

    if (!code.empty()) {
        executor::SmartContractBinary smartContractBinary;
        smartContractBinary.contractAddress = smart_address;
        smartContractBinary.object.byteCodeObjects = code;
        smartContractBinary.object.instance = state;
        smartContractBinary.stateCanModify = solver_.isContractLocked(BlockChain::getAddressFromKey(smart_address)) ? true : false;

        if (auto optOriginRes = execute(address, smartContractBinary, methodHeader, isGetter, sequence)) {
            resp = optOriginRes.value().resp;
        }
    }
}

void cs::Executor::executeByteCodeMultiple(executor::ExecuteByteCodeMultipleResult& _return, const general::Address& initiatorAddress,
                                           const executor::SmartContractBinary& invokedContract, const std::string& method,
                                           const std::vector<std::vector<general::Variant>>& params, const int64_t executionTime, cs::Sequence sequence) {
    const auto accessId = generateAccessId(sequence);

#ifdef USE_EMBEDDED_JVM
    if (embeddedVm_) {
        // Flatten the param matrix and verify every Variant uses a known case.
        std::vector<int32_t> groupSizes;
        groupSizes.reserve(params.size());
        jvm_embed::Vm::VariantInputs flat;
        bool variantsOk = true;
        for (const auto& callParams : params) {
            groupSizes.push_back(static_cast<int32_t>(callParams.size()));
            for (const auto& v : callParams) {
                int32_t tag = jvm_embed::Vm::VTAG_NULL;
                bool b = false; int64_t l = 0; double d = 0.0;
                std::string s; std::vector<uint8_t> by;
                if (!thriftVariantToTagged(v, tag, b, l, d, s, by)) { variantsOk = false; break; }
                flat.tags.push_back(tag);
                flat.bools.push_back(b);
                flat.longs.push_back(l);
                flat.doubles.push_back(d);
                flat.strings.push_back(std::move(s));
                flat.bytes.push_back(std::move(by));
            }
            if (!variantsOk) break;
        }
        if (variantsOk) {
            std::vector<std::string> classNames;
            std::vector<std::vector<uint8_t>> byteCodes;
            for (const auto& bo : invokedContract.object.byteCodeObjects) {
                classNames.push_back(bo.name);
                byteCodes.emplace_back(bo.byteCode.begin(), bo.byteCode.end());
            }
            std::vector<uint8_t> initiator(initiatorAddress.begin(), initiatorAddress.end());
            std::vector<uint8_t> ctrAddr(invokedContract.contractAddress.begin(),
                                        invokedContract.contractAddress.end());
            std::vector<uint8_t> instance(invokedContract.object.instance.begin(),
                                          invokedContract.object.instance.end());

            ++execCount_;
            auto r = embeddedVm_->callExecuteByteCodeMultipleFull(
                static_cast<int64_t>(accessId),
                initiator, ctrAddr,
                classNames, byteCodes,
                instance, invokedContract.stateCanModify,
                method,
                groupSizes, flat,
                static_cast<int64_t>(executionTime),
                EXECUTOR_VERSION);

            if (r) {
                bool allRetValsKnown = true;
                _return.results.clear();
                _return.results.reserve(r->results.size());
                for (const auto& sr : r->results) {
                    executor::GetterMethodResult gmr;
                    gmr.status.code    = sr.code;
                    gmr.status.message = sr.message;
                    if (!taggedToThriftVariant(sr.retVal, gmr.ret_val)) { allRetValsKnown = false; break; }
                    gmr.__isset.status  = true;
                    gmr.__isset.ret_val = true;
                    _return.results.push_back(std::move(gmr));
                }
                if (allRetValsKnown) {
                    _return.status.code    = r->code;
                    _return.status.message = r->message;
                    _return.__isset.status  = true;
                    _return.__isset.results = !_return.results.empty();
                    --execCount_;
                    return;
                }
                csinfo() << "Executor: embedded executeByteCodeMultiple had structural Variant ret_val — falling back to Thrift";
            } else {
                cswarning() << "Executor: embedded executeByteCodeMultiple failed: "
                            << embeddedVm_->lastError() << " — falling back to Thrift";
            }
            --execCount_;
        }
    }
#endif

    if (!isConnected()) {
        _return.status.code = 1;
        _return.status.message = "No executor connection!";

        notifyError();
        return;
    }

    ++execCount_;

    try {
        std::shared_lock lock(sharedErrorMutex_);
        origExecutor_->executeByteCodeMultiple(_return, static_cast<general::AccessID>(accessId), initiatorAddress, invokedContract, method, params, executionTime, EXECUTOR_VERSION);
    }
    catch (::apache::thrift::transport::TTransportException& x) {
        // sets stop_ flag to true forever, replace with new instance
        if (x.getType() == ::apache::thrift::transport::TTransportException::NOT_OPEN) {
            recreateOriginExecutor();
        }

        _return.status.code = 1;
        _return.status.message = x.what();

        notifyError();
    }
    catch (std::exception& x) {
        _return.status.code = 1;
        _return.status.message = x.what();

        notifyError();
    }

    --execCount_;
    deleteAccessId(static_cast<general::AccessID>(accessId));
}

void cs::Executor::getContractMethods(executor::GetContractMethodsResult& _return, const std::vector<general::ByteCodeObject>& byteCodeObjects) {
#ifdef USE_EMBEDDED_JVM
    if (embeddedVm_) {
        std::vector<std::string> names;
        std::vector<std::vector<uint8_t>> bytes;
        names.reserve(byteCodeObjects.size());
        bytes.reserve(byteCodeObjects.size());
        for (const auto& obj : byteCodeObjects) {
            names.push_back(obj.name);
            bytes.emplace_back(obj.byteCode.begin(), obj.byteCode.end());
        }
        auto r = embeddedVm_->callGetContractMethods(names, bytes, EXECUTOR_VERSION);
        if (r) {
            // The executor returns code=1 for legacy contracts that reference
            // unpackaged base classes (e.g. top-level "SmartContract") which
            // don't exist anywhere on the classpath. The legacy executor
            // process fails identically; it just logged the error to its own
            // file. Surface as the same Thrift response shape and don't
            // log-spam at warning level.
            _return.status.code    = r->code;
            _return.status.message = r->message;
            _return.methods.clear();
            _return.methods.reserve(r->methods.size());
            for (const auto& mi : r->methods) {
                general::MethodDescription md;
                md.name       = mi.name;
                md.returnType = mi.returnType;
                md.arguments.reserve(mi.arguments.size());
                for (const auto& mai : mi.arguments) {
                    general::MethodArgument tma;
                    tma.type = mai.type;
                    tma.name = mai.name;
                    tma.annotations.reserve(mai.annotations.size());
                    for (const auto& ai : mai.annotations) {
                        general::Annotation ta;
                        ta.name = ai.name;
                        for (const auto& kv : ai.arguments) ta.arguments[kv.first] = kv.second;
                        ta.__isset.arguments = !ta.arguments.empty();
                        tma.annotations.push_back(std::move(ta));
                    }
                    tma.__isset.annotations = !tma.annotations.empty();
                    md.arguments.push_back(std::move(tma));
                }
                md.annotations.reserve(mi.annotations.size());
                for (const auto& ai : mi.annotations) {
                    general::Annotation ta;
                    ta.name = ai.name;
                    for (const auto& kv : ai.arguments) ta.arguments[kv.first] = kv.second;
                    ta.__isset.arguments = !ta.arguments.empty();
                    md.annotations.push_back(std::move(ta));
                }
                md.__isset.arguments   = !md.arguments.empty();
                md.__isset.annotations = !md.annotations.empty();
                _return.methods.push_back(std::move(md));
            }
            _return.__isset.methods = !_return.methods.empty();
            return;
        }
        cswarning() << "Executor: embedded getContractMethods failed: "
                    << embeddedVm_->lastError() << " — falling back to Thrift";
    }
#endif
    try {
        std::shared_lock lock(sharedErrorMutex_);
        origExecutor_->getContractMethods(_return, byteCodeObjects, EXECUTOR_VERSION);
    }
    catch (const ::apache::thrift::transport::TTransportException& x) {
        // sets stop_ flag to true forever, replace with new instance
        if (x.getType() == ::apache::thrift::transport::TTransportException::NOT_OPEN) {
            recreateOriginExecutor();
        }

        _return.status.code = 1;
        _return.status.message = x.what();

        notifyError();
    }
    catch(const std::exception& x ) {
        _return.status.code = 1;
        _return.status.message = x.what();

        notifyError();
    }
}

void cs::Executor::getContractVariables(executor::GetContractVariablesResult& _return, const std::vector<general::ByteCodeObject>& byteCodeObjects, const std::string& contractState) {
#ifdef USE_EMBEDDED_JVM
    if (embeddedVm_) {
        std::vector<std::string> names;
        std::vector<std::vector<uint8_t>> bytes;
        names.reserve(byteCodeObjects.size());
        bytes.reserve(byteCodeObjects.size());
        for (const auto& obj : byteCodeObjects) {
            names.push_back(obj.name);
            bytes.emplace_back(obj.byteCode.begin(), obj.byteCode.end());
        }
        std::vector<uint8_t> state(contractState.begin(), contractState.end());

        auto r = embeddedVm_->callGetContractVariables(names, bytes, state, EXECUTOR_VERSION);
        if (r) {
            // Convert each TaggedVariant back to Thrift Variant. Fall back
            // to Thrift if any variable is structural (VTAG_OTHER) — losing
            // a Map/List/Object value would silently misreport state.
            bool allKnown = true;
            std::map<std::string, ::general::Variant> rebuilt;
            for (const auto& kv : r->variables) {
                ::general::Variant tv;
                if (!taggedToThriftVariant(kv.second, tv)) { allKnown = false; break; }
                rebuilt[kv.first] = std::move(tv);
            }
            if (allKnown) {
                _return.status.code    = r->code;
                _return.status.message = r->message;
                _return.contractVariables = std::move(rebuilt);
                _return.__isset.contractVariables = !_return.contractVariables.empty();
                return;
            }
            csinfo() << "Executor: embedded getContractVariables had structural Variant — falling back to Thrift";
        } else {
            cswarning() << "Executor: embedded getContractVariables failed: "
                        << embeddedVm_->lastError() << " — falling back to Thrift";
        }
    }
#endif
    try {
        std::shared_lock lock(sharedErrorMutex_);
        origExecutor_->getContractVariables(_return, byteCodeObjects, contractState, EXECUTOR_VERSION);
    }
    catch (const ::apache::thrift::transport::TTransportException& x) {
        // sets stop_ flag to true forever, replace with new instance
        if (x.getType() == ::apache::thrift::transport::TTransportException::NOT_OPEN) {
            recreateOriginExecutor();
        }

        _return.status.code = 1;
        _return.status.message = x.what();

        notifyError();
    }
    catch(const std::exception& x ) {
        _return.status.code = 1;
        _return.status.message = x.what();

        notifyError();
    }
}

void cs::Executor::compileSourceCode(executor::CompileSourceCodeResult& _return, const std::string& sourceCode) {
#ifdef USE_EMBEDDED_JVM
    if (embeddedVm_) {
        auto r = embeddedVm_->callCompileSourceCode(sourceCode, EXECUTOR_VERSION);
        if (r) {
            _return.status.code    = r->code;
            _return.status.message = r->message;
            _return.byteCodeObjects.clear();
            _return.byteCodeObjects.reserve(r->classes.size());
            for (const auto& cls : r->classes) {
                general::ByteCodeObject bo;
                bo.name = cls.name;
                bo.byteCode.assign(reinterpret_cast<const char*>(cls.byteCode.data()),
                                   cls.byteCode.size());
                _return.byteCodeObjects.push_back(std::move(bo));
            }
            return;
        }
        cswarning() << "Executor: embedded compileSourceCode failed: "
                    << embeddedVm_->lastError() << " — falling back to Thrift";
    }
#endif
    try {
        std::shared_lock slk(sharedErrorMutex_);
        origExecutor_->compileSourceCode(_return, sourceCode, EXECUTOR_VERSION);
    }
    catch (::apache::thrift::transport::TTransportException& x) {
        // sets stop_ flag to true forever, replace with new instance
        if (x.getType() == ::apache::thrift::transport::TTransportException::NOT_OPEN) {
            recreateOriginExecutor();
        }

        _return.status.code = 1;
        _return.status.message = x.what();

        notifyError();
    }
    catch(const std::exception& x ) {
        _return.status.code = 1;
        _return.status.message = x.what();

        notifyError();
    }
}

void cs::Executor::getExecutorBuildVersion(executor::ExecutorBuildVersionResult& _return) {
#ifdef USE_EMBEDDED_JVM
    if (embeddedVm_) {
        // Phase 4 cutover: dispatch to the in-process JVM via the bridge.
        // Mirrors the same EmbeddedExecutorBridge call the spike uses.
        auto r = embeddedVm_->callGetExecutorBuildVersion(EXECUTOR_VERSION, "EmbeddedExecutorBridge");
        if (r) {
            _return.status.code     = r->code;
            _return.status.message  = r->message;
            _return.commitNumber    = r->commitNumber;
            _return.commitHash      = r->commitHash;
            return;
        }
        // JVM dispatch failed somehow — log and fall through to Thrift below.
        cswarning() << "Executor: embedded getExecutorBuildVersion failed: "
                    << embeddedVm_->lastError() << " — falling back to Thrift";
    }
#endif
    try {
        std::shared_lock slk(sharedErrorMutex_);
        origExecutor_->getExecutorBuildVersion(_return, EXECUTOR_VERSION);
    }
    catch (::apache::thrift::transport::TTransportException& x) {
        // sets stop_ flag to true forever, replace with new instance
        if (x.getType() == ::apache::thrift::transport::TTransportException::NOT_OPEN) {
            recreateOriginExecutor();
        }

        _return.status.code = 1;
        _return.status.message = x.what();

        notifyError();
    }
    catch (const std::exception& x) {
        _return.status.code = 1;
        _return.status.message = x.what();

        notifyError();
    }
}

cs::Executor& cs::Executor::instance() {
    static Executor executor(cs::ExecutorSettings::get());
    return executor;
}

bool cs::Executor::isConnected() const {
    return executorTransport_->isOpen();
}

void cs::Executor::stop() {
    requestStop_ = true;

#ifdef USE_EMBEDDED_JVM
    // Release the embedded VM early so any dispatcher entering after this
    // point sees it as null and falls through to Thrift (which may itself
    // be torn down — but that's just a logged warning, not a crash). The
    // Vm destructor doesn't call DestroyJavaVM (HotSpot doesn't allow VM
    // recreation in-process anyway); libjvm stays loaded for the rest of
    // the process lifetime, but our wrapper is gone before main() returns.
    if (embeddedVm_) {
        cslog() << "Executor: releasing embedded JVM wrapper";
        embeddedVm_.reset();
    }
#endif

    while (isWatcherRunning_.load(std::memory_order_acquire)) {
        notifyError(); // wake up watching thread if it sleeps
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (executorProcess_) {
        if (executorProcess_->isRunning()) {
            disconnect();
            executorProcess_->terminate();
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    if (manager_.isExecutorProcessRunning()) {
        manager_.stopExecutorProcess();
    }
}

std::optional<cs::Sequence> cs::Executor::getSequence(const general::AccessID& accessId) {
    std::shared_lock lock(mutex_);

    if (auto it = accessSequence_.find(accessId); it != accessSequence_.end()) {
        return std::make_optional(it->second);
    }

    return std::nullopt;
}

std::optional<csdb::TransactionID> cs::Executor::getDeployTrxn(const csdb::Address& address) {
    std::shared_lock lock(mutex_);

    if (const auto it = deployTrxns_.find(address); it != deployTrxns_.end()) {
        return std::make_optional(it->second);
    }

    return std::nullopt;
}

void cs::Executor::updateDeployTrxns(const csdb::Address& address, const csdb::TransactionID& trxnsId) {
    std::lock_guard lock(mutex_);
    deployTrxns_[address] = trxnsId;
}

void cs::Executor::setLastState(const csdb::Address& address, const std::string& state) {
    std::lock_guard lock(mutex_);
    lastState_[address] = state;
}

std::optional<std::string> cs::Executor::getState(const csdb::Address& address) {
    csdb::Address absAddress = blockchain_.getAddressByType(address, BlockChain::AddressType::PublicKey);

    if (!absAddress.is_valid()) {
        return std::nullopt;
    }

    std::string state = cs::SmartContracts::get_contract_state(blockchain_, absAddress);

    if (state.empty()) {
        return std::nullopt;
    }

    return std::make_optional(std::move(state));
}

void cs::Executor::updateCacheLastStates(const csdb::Address& address, const cs::Sequence& sequence, const std::string& state) {
    std::lock_guard lock(mutex_);

    if (execCount_) {
        (cacheLastStates_[address])[sequence] = state;
    }
    else if (cacheLastStates_.size()) {
        cacheLastStates_.clear();
    }
}

std::optional<std::string> cs::Executor::getAccessState(const general::AccessID& accessId, const csdb::Address& address) {
    std::shared_lock lock(mutex_);
    const auto accessSequence = getSequence(accessId);

    if (const auto unmapStatesIter = cacheLastStates_.find(address); unmapStatesIter != cacheLastStates_.end()) {
        std::pair<cs::Sequence, std::string> prevSeqState{};

        for (const auto& [currSeq, currState] : unmapStatesIter->second) {
            if (currSeq > accessSequence) {
                return prevSeqState.first ? std::make_optional<std::string>(prevSeqState.second) : std::nullopt;
            }

            prevSeqState = { currSeq, currState };
        }
    }

    auto lastState = getState(address);
    return lastState.has_value() ? std::make_optional<std::string>(std::move(lastState).value()) : std::nullopt;
}

void cs::Executor::addInnerSendTransaction(const general::AccessID& accessId, const csdb::Transaction& transaction) {
    std::lock_guard lock(mutex_);
    innerSendTransactions_[accessId].push_back(transaction);
}

std::optional<std::vector<csdb::Transaction>> cs::Executor::getInnerSendTransactions(const general::AccessID& accessId) {
    std::shared_lock lock(mutex_);

    if (const auto it = innerSendTransactions_.find(accessId); it != innerSendTransactions_.end()) {
        return std::make_optional(it->second);
    }

    return std::nullopt;
}

void cs::Executor::deleteInnerSendTransactions(const general::AccessID& accessId) {
    std::lock_guard lock(mutex_);
    innerSendTransactions_.erase(accessId);
}

bool cs::Executor::isDeploy(const csdb::Transaction& transaction) {
    if (transaction.user_field(0).is_valid()) {
        const auto sci = cs::Serializer::deserialize<api::SmartContractInvocation>(transaction.user_field(0).value<std::string>());

        if (sci.method.empty()) {
            return true;
        }
    }

    return false;
}

std::optional<cs::Executor::ExecuteResult> cs::Executor::executeTransaction(const std::vector<cs::Executor::ExecuteTransactionInfo>& smarts, std::string forceContractState) {
    if (smarts.empty()) {
        return std::nullopt;
    }

    const auto& headTransaction = smarts[0].transaction;
    const auto& deployTrxn = smarts[0].deploy;

    if (!headTransaction.is_valid() || !deployTrxn.is_valid()) {
        return std::nullopt;
    }

    // all smarts must have the same initiator and address
    const auto source = headTransaction.source();
    const auto target = headTransaction.target();

    for (const auto& smart : smarts) {
        if (source != smart.transaction.source() || target != smart.transaction.target()) {
            return std::nullopt;
        }
    }

    auto smartSource = blockchain_.getAddressByType(source, BlockChain::AddressType::PublicKey);
    auto smartTarget = blockchain_.getAddressByType(target, BlockChain::AddressType::PublicKey);

    // get deploy transaction
    const auto isdeploy = (headTransaction.id() == deployTrxn.id()); //isDeploy(head_transaction);

    // fill smartContractBinary
    const auto sciDeploy = cs::Serializer::deserialize<api::SmartContractInvocation>(deployTrxn.user_field(cs::trx_uf::deploy::Code).value<std::string>());
    executor::SmartContractBinary smartContractBinary;
    smartContractBinary.contractAddress = smartTarget.to_api_addr();
    smartContractBinary.object.byteCodeObjects = sciDeploy.smartContractDeploy.byteCodeObjects;

    // may contain temporary last new state not yet written into block chain (to allow "speculative" multi-executions of the same contract)
    if (!isdeploy) {
        if (!forceContractState.empty()) {
            smartContractBinary.object.instance = forceContractState;
        }
        else {
            auto optState = getState(smartTarget);

            if (optState.has_value()) {
                smartContractBinary.object.instance = optState.value();
            }
        }
    }

    smartContractBinary.stateCanModify = solver_.isContractLocked(smartTarget);

    // fill methodHeaders
    std::vector<executor::MethodHeader> methodHeaders;
    for (const auto& smartItem : smarts) {
        executor::MethodHeader header;
        const csdb::Transaction& smart = smartItem.transaction;

        if (smartItem.convention != MethodNameConvention::Default) {
            // call to payable
            // add method name
            header.methodName = "payable";

            // add arg[0]
            general::Variant& var0 = header.params.emplace_back(::general::Variant{});
            std::string str_val = smart.amount().to_string();

            if (smartItem.convention == MethodNameConvention::PayableLegacy) {
                var0.__set_v_string(str_val);
            }
            else {
                var0.__set_v_big_decimal(str_val);
            }

            // add arg[1]
            str_val.clear();

            using namespace cs::trx_uf;
            if (smart.user_field(ordinary::Text).is_valid()) {
                str_val = smart.user_field(ordinary::Text).value<std::string>();
            }

            general::Variant& var1 = header.params.emplace_back(::general::Variant{});

            if (smartItem.convention == MethodNameConvention::PayableLegacy) {
                var1.__set_v_string(str_val);
            }
            else {
                var1.__set_v_byte_array(str_val);
            }
        }
        else {
            api::SmartContractInvocation sci;
            const auto fld = smart.user_field(cs::trx_uf::start::Methods);

            if (!fld.is_valid()) {
                return std::nullopt;
            }
            else if (!isdeploy) {
                sci = cs::Serializer::deserialize<api::SmartContractInvocation>(fld.value<std::string>());
                header.methodName = sci.method;
                header.params = sci.params;

                for (const auto& addrLock : sci.usedContracts) {
                    addToLockSmart(addrLock, static_cast<general::AccessID>(getFutureAccessId()));
                }
            }
        }

        methodHeaders.push_back(header);
    }

    const auto optOriginRes = execute(smartSource.to_api_addr(), smartContractBinary, methodHeaders, false /*isGetter*/, smarts[0].sequence /*sequence*/, headTransaction.get_time());

    for (const auto& smart : smarts) {
        if (!isdeploy) {
            if (smart.convention == MethodNameConvention::Default) {
                const auto fld = smart.transaction.user_field(0);
                if (fld.is_valid()) {
                    auto sci = cs::Serializer::deserialize<api::SmartContractInvocation>(smart.transaction.user_field(0).value<std::string>());
                    for (const auto& addrLock : sci.usedContracts) {
                        deleteFromLockSmart(addrLock, static_cast<general::AccessID>(getFutureAccessId()));
                    }
                }
            }
        }
    }

    if (!optOriginRes.has_value()) {
        return std::nullopt;
    }

    // fill res
    ExecuteResult res;
    res.response = optOriginRes.value().resp.status;

    deleteInnerSendTransactions(optOriginRes.value().acceessId);
    res.selfMeasuredCost = static_cast<long>(optOriginRes.value().timeExecute);

    for (const auto& setters : optOriginRes.value().resp.results) {
        auto& smartRes = res.smartsRes.emplace_back(ExecuteResult::SmartRes{});
        smartRes.retValue = setters.ret_val;
        smartRes.executionCost = setters.executionCost;
        smartRes.response = setters.status;

        for (auto& states : setters.contractsState) {  // state
            auto addr = BlockChain::getAddressFromKey(states.first);
            smartRes.states[BlockChain::getAddressFromKey(states.first)] = states.second;
        }

        for (auto transaction : setters.emittedTransactions) {  // emittedTransactions
            ExecuteResult::EmittedTrxn emittedTrxn;
            emittedTrxn.source = BlockChain::getAddressFromKey(transaction.source);
            emittedTrxn.target = BlockChain::getAddressFromKey(transaction.target);
            emittedTrxn.amount = csdb::Amount(transaction.amount.integral, static_cast<uint64_t>(transaction.amount.fraction));
            emittedTrxn.userData = transaction.userData;
            smartRes.emittedTransactions.push_back(emittedTrxn);
        }
    }

    return std::make_optional(std::move(res));
}

std::optional<cs::Executor::ExecuteResult> cs::Executor::reexecuteContract(cs::Executor::ExecuteTransactionInfo& contract, std::string forceContractState) {
    if (!contract.transaction.is_valid() || !contract.deploy.is_valid()) {
        return std::nullopt;
    }

    auto smartSource = blockchain_.getAddressByType(contract.transaction.source(), BlockChain::AddressType::PublicKey);
    auto smartTarget = blockchain_.getAddressByType(contract.transaction.target(), BlockChain::AddressType::PublicKey);

    // get deploy transaction
    const csdb::Transaction& deployTrxn = contract.deploy;
    const auto isdeploy = (contract.deploy.id() == contract.transaction.id()); // isDeploy(contract.transaction);

    // fill smartContractBinary
    const auto sci_deploy = cs::Serializer::deserialize<api::SmartContractInvocation>(deployTrxn.user_field(0).value<std::string>());
    executor::SmartContractBinary smartContractBinary;
    smartContractBinary.contractAddress = smartTarget.to_api_addr();
    smartContractBinary.object.byteCodeObjects = sci_deploy.smartContractDeploy.byteCodeObjects;

    // may contain temporary last new state not yet written into block chain (to allow "speculative" multi-executions af the same contract)
    if (!isdeploy) {
        if (!forceContractState.empty()) {
            smartContractBinary.object.instance = forceContractState;
        }
        else {
            auto optState = getState(smartTarget);
            if (optState.has_value()) {
                smartContractBinary.object.instance = optState.value();
            }
        }
    }
    smartContractBinary.stateCanModify = true;

    // fill methodHeaders
    std::vector<executor::MethodHeader> methodHeaders;
    executor::MethodHeader header;

    if (contract.convention != MethodNameConvention::Default) {
        // call to payable
        // add method name
        header.methodName = "payable";

        // add arg[0]
        general::Variant& var0 = header.params.emplace_back(::general::Variant{});
        std::string str_val = contract.transaction.amount().to_string();

        if (contract.convention == MethodNameConvention::PayableLegacy) {
            var0.__set_v_string(str_val);
        }
        else {
            var0.__set_v_big_decimal(str_val);
        }

        // add arg[1]
        str_val.clear();

        using namespace cs::trx_uf;
        if (contract.transaction.user_field(ordinary::Text).is_valid()) {
            str_val = contract.transaction.user_field(ordinary::Text).value<std::string>();
        }

        general::Variant& var1 = header.params.emplace_back(::general::Variant{});

        if (contract.convention == MethodNameConvention::PayableLegacy) {
            var1.__set_v_string(str_val);
        }
        else {
            var1.__set_v_byte_array(str_val);
        }
    }
    else {
        api::SmartContractInvocation sci;
        const auto fld = contract.transaction.user_field(0);

        if (!fld.is_valid()) {
            return std::nullopt;
        }
        else if (!isdeploy) {
            sci = cs::Serializer::deserialize<api::SmartContractInvocation>(fld.value<std::string>());
            header.methodName = sci.method;
            header.params = sci.params;

            for (const auto& addrLock : sci.usedContracts) {
                addToLockSmart(addrLock, static_cast<general::AccessID>(getFutureAccessId()));
            }
        }
    }

    methodHeaders.push_back(header);

    const auto optOriginRes = execute(smartSource.to_api_addr(), smartContractBinary, methodHeaders, false /*! isGetter*/, contract.sequence);

    if (!isdeploy) {
        if (contract.convention == MethodNameConvention::Default) {
            const auto fld = contract.transaction.user_field(0);
            if (fld.is_valid()) {
                auto sci = cs::Serializer::deserialize<api::SmartContractInvocation>(contract.transaction.user_field(0).value<std::string>());
                for (const auto& addrLock : sci.usedContracts) {
                    deleteFromLockSmart(addrLock, static_cast<general::AccessID>(getFutureAccessId()));
                }
            }
        }
    }

    if (!optOriginRes.has_value()) {
        return std::nullopt;
    }

    // fill res
    ExecuteResult res;
    res.response = optOriginRes.value().resp.status;

    deleteInnerSendTransactions(optOriginRes.value().acceessId);
    res.selfMeasuredCost = static_cast<long>(optOriginRes.value().timeExecute);

    for (const auto& setters : optOriginRes.value().resp.results) {
        auto& smartRes = res.smartsRes.emplace_back(ExecuteResult::SmartRes{});
        smartRes.retValue = setters.ret_val;
        smartRes.executionCost = setters.executionCost;
        smartRes.response = setters.status;

        for (auto& states : setters.contractsState) {          // state
            auto addr = BlockChain::getAddressFromKey(states.first);
            smartRes.states[BlockChain::getAddressFromKey(states.first)] = states.second;
        }

        for (auto transaction : setters.emittedTransactions) { // emittedTransactions
            ExecuteResult::EmittedTrxn emittedTrxn;
            emittedTrxn.source = BlockChain::getAddressFromKey(transaction.source);
            emittedTrxn.target = BlockChain::getAddressFromKey(transaction.target);
            emittedTrxn.amount = csdb::Amount(transaction.amount.integral, static_cast<uint64_t>(transaction.amount.fraction));
            emittedTrxn.userData = transaction.userData;
            smartRes.emittedTransactions.push_back(emittedTrxn);
        }
    }

    return std::make_optional(std::move(res));
}

csdb::Transaction cs::Executor::makeTransaction(const api::Transaction& transaction) {
    csdb::Transaction sendTransaction;
    const auto source = BlockChain::getAddressFromKey(transaction.source);
    const uint64_t walletDenom = csdb::Amount::AMOUNT_MAX_FRACTION;  // 1'000'000'000'000'000'000ull;

    sendTransaction.set_amount(csdb::Amount(transaction.amount.integral, uint64_t(transaction.amount.fraction), walletDenom));

    BlockChain::WalletData dummy{};

    if (!blockchain_.findWalletData(source, dummy)) {
        return csdb::Transaction{}; // disable transaction from unknown source!
    }

    sendTransaction.set_currency(csdb::Currency(1));
    sendTransaction.set_source(source);
    sendTransaction.set_target(BlockChain::getAddressFromKey(transaction.target));
    sendTransaction.set_max_fee(csdb::AmountCommission(uint16_t(transaction.fee.commission)));
    sendTransaction.set_innerID(transaction.id & 0x3fffffffffff);

    // TODO Change Thrift to avoid copy
    cs::Signature signature;
    if (transaction.signature.size() == signature.size()) {
        std::copy(transaction.signature.begin(), transaction.signature.end(), signature.begin());
    }
    else {
        signature.fill(0);
    }

    sendTransaction.set_signature(signature);
    return sendTransaction;
}

void cs::Executor::stateUpdate(const csdb::Pool& pool) {
    if (!pool.transactions().size()) {
        return;
    }

    for (const auto& trxn : pool.transactions()) {
        if (trxn.is_valid() && cs::SmartContracts::is_state_updated(trxn)) {
            const auto address = blockchain_.getAddressByType(trxn.target(), BlockChain::AddressType::PublicKey);
            const auto newstate = cs::SmartContracts::get_contract_state(blockchain_, address);

            if (!newstate.empty()) {
                setLastState(address, newstate);
                updateCacheLastStates(address, pool.sequence(), newstate);
            }
        }
    }
}

void cs::Executor::addToLockSmart(const general::Address& address, const general::AccessID& accessId) {
    std::lock_guard lock(mutex_);
    lockSmarts_[address] = accessId;
}

void cs::Executor::deleteFromLockSmart(const general::Address& address, const general::AccessID& accessId) {
    csunused(accessId);
    std::lock_guard lock(mutex_);
    lockSmarts_.erase(address);
}

bool cs::Executor::isLockSmart(const general::Address& address, const general::AccessID& accessId) {
    std::lock_guard lock(mutex_);

    if (auto addrLock = lockSmarts_.find(address); addrLock != lockSmarts_.end() && addrLock->second == accessId) {
        return true;
    }

    return false;
}

csdb::Transaction cs::Executor::loadTransactionApi(const csdb::TransactionID& id) const {
    std::lock_guard lock(blockMutex_);
    return blockchain_.loadTransaction(id);
}

uint64_t cs::Executor::getTimeSmartContract(general::AccessID accessId) {
    std::lock_guard lock(mutex_);   
    if (auto it = executeTrxnsTime.find(accessId); it != executeTrxnsTime.end()) 
        return it->second;
    return 0;
}

void cs::Executor::onBlockStored(const csdb::Pool& pool) {
    stateUpdate(pool);
}

void cs::Executor::onReadBlock(const csdb::Pool& pool) {
    stateUpdate(pool);
}

void cs::Executor::onExecutorStarted() {
    if (!isConnected()) {
        connect();
    }

    csdebug() << csname() << "started";
}

void cs::Executor::onExecutorFinished(int code, const std::error_code&) {
    if (requestStop_) {
        return;
    }

    if (!executorMessages_.count(code)) {
        cswarning() << "Executor unknown error";
    }
    else {
        cswarning() << executorMessages_[code];
    }

    if (code == ExecutorErrorCode::ServerStartError ||
        code == ExecutorErrorCode::IncorrecJdkVersion) {
        return;
    }

    notifyError();
}

void cs::Executor::onExecutorProcessError(const cs::ProcessException& exception) {
    cswarning() << "Executor process error occured " << exception.what() << ", code " << exception.code();
}

void cs::Executor::checkExecutorVersion() {
    executor::ExecutorBuildVersionResult _return;
    bool result = false;

    std::this_thread::sleep_for(std::chrono::milliseconds(cs::ConfigHolder::instance().config()->getApiSettings().executorCheckVersionDelay));

    if (requestStop_) {
        return;
    }

    connect();
    getExecutorBuildVersion(_return);

    if (_return.status.code != 0) {
        cserror() << "Start contract executor error code " << int(_return.status.code) << ": " << _return.status.message;
    }
    else {
        cslog() << "Start contract executor: " << _return.status.message << ". Executor build number " << _return.commitNumber;
    }

    result = _return.commitNumber < commitMin_ || (_return.commitNumber > commitMax_ && commitMax_ != -1);
    csdebug() << "[executorInfo]: commitNumber: " << _return.commitNumber << ", commitHash: " << _return.commitHash;

    if (result) {
        if (commitMax_ != -1) {
            cserror() << "Executor commit number: " << _return.commitNumber << " is out of range (" << commitMin_ << " .. " << commitMax_ << ")";
        }
        else {
            cserror() << "Executor commit number: " << _return.commitNumber << " is out of range (" << commitMin_ << " .. any)";
        }

        auto terminate = [this] {
            executorProcess_->terminate();
            notifyError();
        };

        cs::Concurrent::run(cs::ConcurrentPolicy::Thread, terminate);
    }
}

cs::Executor::Executor(const cs::ExecutorSettings::Types& types)
: blockchain_(std::get<cs::Reference<const BlockChain>>(types))
, solver_(std::get<cs::Reference<const cs::SolverCore>>(types))
, socket_(::apache::thrift::stdcxx::make_shared<::apache::thrift::transport::TSocket>(cs::ConfigHolder::instance().config()->getApiSettings().executorHost,
                                                                                      cs::ConfigHolder::instance().config()->getApiSettings().executorPort))
, executorTransport_(new ::apache::thrift::transport::TBufferedTransport(socket_))
, origExecutor_(std::make_unique<executor::ContractExecutorConcurrentClient>(::apache::thrift::stdcxx::make_shared<apache::thrift::protocol::TBinaryProtocol>(executorTransport_))) {
    socket_->setSendTimeout(cs::ConfigHolder::instance().config()->getApiSettings().executorSendTimeout);
    socket_->setRecvTimeout(cs::ConfigHolder::instance().config()->getApiSettings().executorReceiveTimeout);

    commitMin_ = cs::ConfigHolder::instance().config()->getApiSettings().executorCommitMin;
    commitMax_ = cs::ConfigHolder::instance().config()->getApiSettings().executorCommitMax;

#ifdef USE_EMBEDDED_JVM
    // Phase 4 cutover: spin up the in-process JVM if the config asks for it.
    // On any failure we leave embeddedVm_ null and fall back to the Thrift
    // path — non-fatal so an operator with a misconfigured embedded mode
    // still gets a working node via the legacy executor process.
    {
        const auto& api = cs::ConfigHolder::instance().config()->getApiSettings();
        if (api.useEmbeddedJvm) {
            // Resolve the JDK path explicitly so the operator can see exactly
            // which JVM is being loaded. Fall back to JAVA_HOME env var with a
            // warning — this is a footgun (e.g. Android Studio's bundled JBR
            // is JDK 17 but our contract-executor.jar is built for JDK 11,
            // and the version mismatch crashes inside jimage.dll on startup).
            std::string resolvedJavaHome = api.embeddedJvmJavaHome;
            const char* envJavaHome = std::getenv("JAVA_HOME");
            if (resolvedJavaHome.empty()) {
                if (envJavaHome && *envJavaHome) {
                    resolvedJavaHome = envJavaHome;
                    cswarning() << "Executor: embedded_jvm_java_home not set; "
                                << "using JAVA_HOME='" << resolvedJavaHome
                                << "' — explicit setting in config.ini is recommended.";
                } else {
                    cserror() << "Executor: cannot start embedded JVM: neither "
                              << "embedded_jvm_java_home (config) nor JAVA_HOME (env) is set";
                }
            }
            cslog() << "Executor: starting embedded JVM"
                    << " (java_home=" << (resolvedJavaHome.empty() ? "<unset>" : resolvedJavaHome)
                    << ", executor_jar=" << api.embeddedJvmExecutorJar << ")";

            auto vm = std::make_unique<jvm_embed::Vm>();
            jvm_embed::InitOptions opts;
            opts.javaHomeOverride = resolvedJavaHome;
            // Bridge classes are staged next to node.exe at build time.
            opts.classpath.emplace_back("./jvm_bridge");
            if (!api.embeddedJvmExecutorJar.empty())
                opts.classpath.push_back(api.embeddedJvmExecutorJar);
            if (!api.embeddedJvmInstallDir.empty())
                opts.classpath.push_back(api.embeddedJvmInstallDir);
            if (!api.embeddedJvmScapiJar.empty())
                opts.classpath.push_back(api.embeddedJvmScapiJar);
            // -Xrs tells the JVM not to install its own SIGINT/SIGTERM handlers.
            // Without this, the JVM's handler races the C runtime's on Ctrl-C
            // and the process crashes during teardown. The host (node.exe) keeps
            // ownership of console signals.
            opts.jvmOptions.emplace_back("-Xrs");
            for (const auto& opt : api.embeddedJvmOptions) {
                opts.jvmOptions.push_back(opt);
            }
            if (vm->start(opts)) {
                cslog() << "Executor: embedded JVM started successfully";

                // Run the Variant marshalling self-test. This catches missing
                // flattenVariant cases, stale tag-clamps, and per-case
                // marshalling bugs at startup — before they show up as a
                // flood of fallback warnings on the live chain.
                auto report = vm->runVariantSelfTest();
                if (!report) {
                    cswarning() << "Executor: Variant self-test could not run: "
                                << vm->lastError() << " — disabling embedded path";
                } else if (!report->allOk()) {
                    cswarning() << "Executor: Variant self-test FAILED ("
                                << report->fails << " of " << report->cases.size()
                                << " cases) — disabling embedded path:";
                    for (const auto& c : report->cases) {
                        if (!c.ok) {
                            cswarning() << "  case '" << c.name
                                        << "' expected tag=" << c.expectedTag
                                        << " but got tag=" << c.actualTag;
                        }
                    }
                } else {
                    cslog() << "Executor: Variant self-test passed ("
                            << report->passes << " of " << report->cases.size()
                            << " cases)";
                    embeddedVm_ = std::move(vm);
                }
            } else {
                cswarning() << "Executor: embedded JVM start failed: " << vm->lastError()
                            << " — falling back to Thrift executor process";
            }
        }
    }
#endif

    if (cs::ConfigHolder::instance().config()->getApiSettings().executorCmdLine.empty()) {
        cswarning() << "Executor command line args are empty, process would not be created";
        return;
    }

    executorProcess_ = std::make_unique<cs::Process>(cs::ConfigHolder::instance().config()->getApiSettings().executorCmdLine);

    cs::Connector::connect(&executorProcess_->started, this, &Executor::onExecutorStarted);
    cs::Connector::connect(&executorProcess_->finished, this, &Executor::onExecutorFinished);
    cs::Connector::connect(&executorProcess_->errorOccured, this, &Executor::onExecutorProcessError);
    cs::Connector::connect(&executorProcess_->started, this, &Executor::checkExecutorVersion);

    checkAnotherExecutor();
    executorProcess_->launch(cs::Process::Options::None);

    std::this_thread::sleep_for(std::chrono::milliseconds(cs::ConfigHolder::instance().config()->getApiSettings().executorRunDelay));
    state_ = executorProcess_->isRunning() ? ExecutorState::Launched : ExecutorState::Idle;

    if (state_ == ExecutorState::Idle) {
        cswarning() << "Executor can not start, watcher thread would not be created";
        return;
    }

    auto watcher = [this]() {
        isWatcherRunning_.store(true, std::memory_order_release);

        while (!requestStop_) {
            if (isConnected()) {
                static std::mutex mutex;
                std::unique_lock lock(mutex);

                cvErrorConnect_.wait_for(lock, std::chrono::seconds(5), [&] {
                    return !isConnected() || requestStop_;
                });
            }

            if (requestStop_) {
                break;
            }

            if (executorProcess_->isRunning()) {
                if (!isConnected()) {
                    connect();
                }
            }
            else if (state_ != ExecutorState::Launching) {
                checkAnotherExecutor();
                runProcess();
            }
        }

        isWatcherRunning_.store(false, std::memory_order_release);
        cslog() << "Executor watcher thread finished";
    };

    cs::Concurrent::run(cs::ConcurrentPolicy::Thread, watcher);
}

cs::Executor::~Executor() {
    if (!requestStop_) {
        stop();
    }
}

void cs::Executor::runProcess() {
    state_ = ExecutorState::Launching;

    executorProcess_->terminate();

    std::this_thread::sleep_for(std::chrono::milliseconds(cs::ConfigHolder::instance().config()->getApiSettings().executorRunDelay));

    executorProcess_->setProgram(cs::ConfigHolder::instance().config()->getApiSettings().executorCmdLine);
    executorProcess_->launch(cs::Process::Options::None);

    state_ = ExecutorState::Launched;
}

void cs::Executor::checkAnotherExecutor() {
    if (!cs::ConfigHolder::instance().config()->getApiSettings().executorMultiInstance) {
        manager_.stopExecutorProcess();
    }
}

uint64_t cs::Executor::generateAccessId(cs::Sequence explicitSequence, uint64_t time) {
    std::lock_guard lock(mutex_);
    ++lastAccessId_;
    accessSequence_[lastAccessId_] = (explicitSequence != kUseLastSequence ? explicitSequence : blockchain_.getLastSeq());

    if (time) {
        executeTrxnsTime[lastAccessId_] = time;
    }
    else {
        // try to get time from block
        const csdb::Pool block = blockchain_.loadBlock(accessSequence_[lastAccessId_]);
        if (block.is_valid()) {
            executeTrxnsTime[lastAccessId_] = BlockChain::getBlockTime(block);
        }
    }

    return static_cast<uint64_t>(lastAccessId_);
}

uint64_t cs::Executor::getFutureAccessId() {
    return static_cast<uint64_t>(lastAccessId_ + 1);
}

void cs::Executor::deleteAccessId(const general::AccessID& accessId) {
    std::lock_guard lock(mutex_);
    accessSequence_.erase(accessId);
    executeTrxnsTime.erase(accessId);
}

std::optional<cs::Executor::OriginExecuteResult> cs::Executor::execute(const std::string& address, const executor::SmartContractBinary& smartContractBinary,
                                                                       std::vector<executor::MethodHeader>& methodHeader, bool isGetter, cs::Sequence explicitSequence, uint64_t time) {
    const uint64_t EXECUTION_TIME = Consensus::TimeSmartContract;
    OriginExecuteResult originExecuteRes{};

    if (!isConnected()) {
        notifyError();
        return std::nullopt;
    }

    uint64_t accessId{};

    if (!isGetter) {
        accessId = generateAccessId(explicitSequence, time);
    }

    ++execCount_;

    const auto timeBeg = std::chrono::steady_clock::now();

#ifdef USE_EMBEDDED_JVM
    // Dispatch through the embedded JVM when:
    //   1. Embedded VM is up.
    //   2. Every parameter Variant uses a case the bridge recognises.
    //      Unknown-case fall-back protects against silently dropping a value.
    // Multi-method headers are now supported (one MethodHeader per entry).
    bool embeddedHandled = false;
    if (embeddedVm_ && !methodHeader.empty()) {
        std::vector<std::string> methodNames;
        std::vector<int32_t>     paramGroupSizes;
        methodNames.reserve(methodHeader.size());
        paramGroupSizes.reserve(methodHeader.size());

        jvm_embed::Vm::VariantInputs vparams;
        bool variantsOk = true;
        for (const auto& header : methodHeader) {
            methodNames.push_back(header.methodName);
            paramGroupSizes.push_back(static_cast<int32_t>(header.params.size()));
            for (const auto& v : header.params) {
                int32_t tag = jvm_embed::Vm::VTAG_NULL;
                bool b = false; int64_t l = 0; double d = 0.0;
                std::string s; std::vector<uint8_t> by;
                if (!thriftVariantToTagged(v, tag, b, l, d, s, by)) { variantsOk = false; break; }
                vparams.tags.push_back(tag);
                vparams.bools.push_back(b);
                vparams.longs.push_back(l);
                vparams.doubles.push_back(d);
                vparams.strings.push_back(std::move(s));
                vparams.bytes.push_back(std::move(by));
            }
            if (!variantsOk) break;
        }
        if (variantsOk) {
            // Flatten SmartContractBinary's class list to plain parallel arrays.
            std::vector<std::string> classNames;
            std::vector<std::vector<uint8_t>> byteCodes;
            classNames.reserve(smartContractBinary.object.byteCodeObjects.size());
            byteCodes.reserve(smartContractBinary.object.byteCodeObjects.size());
            for (const auto& bo : smartContractBinary.object.byteCodeObjects) {
                classNames.push_back(bo.name);
                byteCodes.emplace_back(bo.byteCode.begin(), bo.byteCode.end());
            }
            std::vector<uint8_t> initiator(address.begin(), address.end());
            std::vector<uint8_t> ctrAddr(smartContractBinary.contractAddress.begin(),
                                        smartContractBinary.contractAddress.end());
            std::vector<uint8_t> instance(smartContractBinary.object.instance.begin(),
                                          smartContractBinary.object.instance.end());

            auto r = embeddedVm_->callExecuteByteCodeFull(
                static_cast<int64_t>(accessId),
                initiator, ctrAddr,
                classNames, byteCodes,
                instance, smartContractBinary.stateCanModify,
                methodNames, paramGroupSizes, vparams,
                static_cast<int64_t>(EXECUTION_TIME),
                EXECUTOR_VERSION);

            if (r) {
                // Populate Thrift result with full fidelity.
                auto& resp = originExecuteRes.resp;
                resp.status.code    = r->code;
                resp.status.message = r->message;
                resp.results.clear();
                resp.results.reserve(r->results.size());
                for (const auto& sr : r->results) {
                    executor::SetterMethodResult tsr;
                    tsr.status.code    = sr.code;
                    tsr.status.message = sr.message;
                    tsr.executionCost  = sr.executionCost;

                    // ret_val: tagged variant -> Thrift Variant.
                    switch (sr.retVal.tag) {
                        case jvm_embed::Vm::VTAG_BOOL:    tsr.ret_val.__set_v_boolean(sr.retVal.boolVal); break;
                        case jvm_embed::Vm::VTAG_LONG:    tsr.ret_val.__set_v_long(sr.retVal.longVal); break;
                        case jvm_embed::Vm::VTAG_DOUBLE:  tsr.ret_val.__set_v_double(sr.retVal.doubleVal); break;
                        case jvm_embed::Vm::VTAG_STRING:  tsr.ret_val.__set_v_string(sr.retVal.stringVal); break;
                        case jvm_embed::Vm::VTAG_BYTES:   tsr.ret_val.__set_v_byte_array(std::string(sr.retVal.bytesVal.begin(), sr.retVal.bytesVal.end())); break;
                        case jvm_embed::Vm::VTAG_VOID:    tsr.ret_val.__set_v_void(0); break;
                        case jvm_embed::Vm::VTAG_NULL:    tsr.ret_val.__set_v_null(""); break;
                        case jvm_embed::Vm::VTAG_AMOUNT: {
                            general::Amount amt;
                            amt.integral = static_cast<int32_t>(sr.retVal.longVal);
                            int64_t frac = 0;
                            if (sr.retVal.bytesVal.size() == 8) {
                                for (size_t i = 0; i < 8; ++i) frac = (frac << 8) | int64_t(sr.retVal.bytesVal[i]);
                            }
                            amt.fraction = frac;
                            tsr.ret_val.__set_v_amount(amt);
                            break;
                        }
                        case jvm_embed::Vm::VTAG_OTHER:
                        default:
                            // V_LIST/V_MAP/V_OBJECT/V_AMOUNT/V_ARRAY — bridge can't
                            // reconstruct these recursive cases. Fall back to Thrift
                            // so the chain doesn't see a wrong ret_val. Logged at
                            // info level (not warning) since some methods legitimately
                            // return these and this is expected behaviour.
                            embeddedHandled = false;
                            csinfo() << "Executor: embedded execute returned VTAG_OTHER ret_val "
                                     << "(repr=\"" << sr.retVal.repr.substr(0, 60)
                                     << "\") — falling back to Thrift to preserve fidelity";
                            goto fallback_to_thrift;
                    }

                    // contractsState: vector of (address, state) -> map.
                    for (const auto& se : sr.contractsState) {
                        std::string addrStr(se.address.begin(), se.address.end());
                        std::string stateStr(se.state.begin(),   se.state.end());
                        tsr.contractsState[addrStr] = std::move(stateStr);
                    }
                    tsr.__isset.contractsState = !tsr.contractsState.empty();

                    // emittedTransactions.
                    tsr.emittedTransactions.reserve(sr.emittedTransactions.size());
                    for (const auto& et : sr.emittedTransactions) {
                        executor::EmittedTransaction tet;
                        tet.source   = std::string(et.source.begin(), et.source.end());
                        tet.target   = std::string(et.target.begin(), et.target.end());
                        tet.amount.integral = et.amountIntegral;
                        tet.amount.fraction = et.amountFraction;
                        tet.userData = std::string(et.userData.begin(), et.userData.end());
                        tet.__isset.amount = true;
                        tet.__isset.userData = !tet.userData.empty();
                        tsr.emittedTransactions.push_back(std::move(tet));
                    }
                    tsr.__isset.emittedTransactions = !tsr.emittedTransactions.empty();

                    tsr.__isset.status = true;
                    tsr.__isset.ret_val = true;
                    tsr.__isset.executionCost = true;
                    resp.results.push_back(std::move(tsr));
                }
                resp.__isset.status = true;
                resp.__isset.results = !resp.results.empty();
                embeddedHandled = true;
            } else {
                cswarning() << "Executor: embedded executeByteCode failed: "
                            << embeddedVm_->lastError() << " — falling back to Thrift";
            }
        }
    }
fallback_to_thrift:
    if (embeddedHandled) {
        // Embedded path succeeded. Skip the Thrift call. Mirror the same
        // bookkeeping the Thrift path does at function tail.
        originExecuteRes.timeExecute = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - timeBeg).count();
        --execCount_;
        if (!isGetter) {
            deleteAccessId(static_cast<general::AccessID>(accessId));
        }
        originExecuteRes.acceessId = static_cast<general::AccessID>(accessId);
        return std::make_optional(std::move(originExecuteRes));
    }
#endif

    try {
        std::shared_lock sharedLock(sharedErrorMutex_);
        std::lock_guard lock(callExecutorLock_);
        origExecutor_->executeByteCode(originExecuteRes.resp, static_cast<general::AccessID>(accessId), address, smartContractBinary, methodHeader, EXECUTION_TIME, EXECUTOR_VERSION);
    }
    catch (::apache::thrift::transport::TTransportException& x) {
        // sets stop_ flag to true forever, replace with new instance
        if (x.getType() == ::apache::thrift::transport::TTransportException::NOT_OPEN) {
            recreateOriginExecutor();
        }

        if (x.getType() == ::apache::thrift::transport::TTransportException::TIMED_OUT) {
            originExecuteRes.resp.status.code = cs::error::TimeExpired;
        }
        else {
            originExecuteRes.resp.status.code = cs::error::ThriftException;
        }
        originExecuteRes.resp.status.message = x.what();

        notifyError();
    }
    catch (std::exception& x) {
        originExecuteRes.resp.status.code = cs::error::StdException;
        originExecuteRes.resp.status.message = x.what();

        notifyError();
    }

    originExecuteRes.timeExecute = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - timeBeg).count();
    --execCount_;

    if (!isGetter) {
        deleteAccessId(static_cast<general::AccessID>(accessId));
    }

    originExecuteRes.acceessId = static_cast<general::AccessID>(accessId);
    return std::make_optional(std::move(originExecuteRes));
}

bool cs::Executor::connect() {
    try {
        executorTransport_->open();
    }
    catch (...) {
        notifyError();
    }

    return executorTransport_->isOpen();
}

void cs::Executor::disconnect() {
    try {
        executorTransport_->close();
    }
    catch (::apache::thrift::transport::TTransportException&) {
        notifyError();
    }
}

void cs::Executor::notifyError() {
    if (isConnected()) {
        disconnect();
    }

    cvErrorConnect_.notify_one();
}

void cs::Executor::recreateOriginExecutor() {
    std::lock_guard lock(sharedErrorMutex_);
    disconnect();
    origExecutor_.reset(new executor::ContractExecutorConcurrentClient(::apache::thrift::stdcxx::make_shared<apache::thrift::protocol::TBinaryProtocol>(executorTransport_)));
}
