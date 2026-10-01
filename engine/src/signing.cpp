#include "signing.h"
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFWriter.hh>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>
#include <sstream>
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <Security/CMSEncoder.h>
#include <Security/CMSDecoder.h>
#include <unistd.h>
#include <cstdlib>
#endif

namespace pdfengine::signing {
namespace {
using Obj = QPDFObjectHandle;
constexpr std::size_t signatureCapacity = 16384; // DER bytes reserved for the CMS blob.

std::string hexOf(const Bytes& data) {
    static const char* digits = "0123456789ABCDEF"; std::string out;
    for (auto b : data) { out += digits[b >> 4]; out += digits[b & 15]; }
    return out;
}
std::string pad10(std::size_t v) { auto s = std::to_string(v); return std::string(10 - std::min<std::size_t>(10, s.size()), '0') + s; }
std::string pdfDate() {
    std::time_t now = std::time(nullptr); std::tm utc{}; gmtime_r(&now, &utc);
    char buffer[32]; std::strftime(buffer, sizeof buffer, "D:%Y%m%d%H%M%SZ", &utc);
    return buffer;
}
std::string unparsed(Obj object) { return object.unparseResolved(); }

#ifdef __APPLE__
template <class T> struct Ref {
    T value{}; Ref() = default; explicit Ref(T v) : value(v) {}
    Ref(const Ref&) = delete; Ref& operator=(const Ref&) = delete;
    Ref& operator=(Ref&& other) noexcept { std::swap(value, other.value); return *this; }
    Ref(Ref&& other) noexcept : value(other.value) { other.value = nullptr; }
    ~Ref() { if (value) CFRelease(value); }
    explicit operator bool() const { return value != nullptr; }
};
Ref<CFDataRef> dataOf(const unsigned char* p, std::size_t n) { return Ref<CFDataRef>(CFDataCreate(nullptr, p, static_cast<CFIndex>(n))); }

// Imports the .p12 into a throw-away keychain so the user's keychain is never touched.
struct Identity {
    Ref<SecKeychainRef> keychain; std::string path; SecIdentityRef identity{};
    Ref<CFArrayRef> items;
    Identity(const std::filesystem::path& p12, const std::string& password) {
        try { open(p12, password); } catch (...) { cleanup(); throw; }
    }
    void open(const std::filesystem::path& p12, const std::string& password) {
        Bytes raw;
        try { raw = readFile(p12); } catch (const Error&) { throw Error(ErrorCode::InvalidSelection, "The certificate file could not be opened."); }
        char tmpl[] = "/tmp/folioforge-sign-XXXXXX";
        if (!mkdtemp(tmpl)) throw Error(ErrorCode::SaveFailed, "Could not create a temporary keychain.");
        path = std::string(tmpl) + "/sign.keychain";
        SecKeychainRef kc{};
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (SecKeychainCreate(path.c_str(), 8, "folioforge", false, nullptr, &kc) != errSecSuccess) throw Error(ErrorCode::SaveFailed, "Could not create a temporary keychain.");
        keychain = Ref<SecKeychainRef>(kc);
        auto data = dataOf(raw.data(), raw.size());
        Ref<CFStringRef> pass(CFStringCreateWithCString(nullptr, password.c_str(), kCFStringEncodingUTF8));
        const void* keys[] = {kSecImportExportPassphrase, kSecImportExportKeychain};
        const void* values[] = {pass.value, kc};
        Ref<CFDictionaryRef> options(CFDictionaryCreate(nullptr, keys, values, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
        CFArrayRef imported{};
        auto status = SecPKCS12Import(data.value, options.value, &imported);
        items = Ref<CFArrayRef>(imported);
        if (status == errSecAuthFailed || status == errSecDecode || status == errSecPkcs12VerifyFailure) throw Error(ErrorCode::InvalidSelection, "The certificate file could not be opened. Check the password.");
        if (status != errSecSuccess || !imported || CFArrayGetCount(imported) < 1) throw Error(ErrorCode::InvalidSelection, "The certificate file could not be read.");
        auto entry = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(imported, 0));
        identity = static_cast<SecIdentityRef>(const_cast<void*>(CFDictionaryGetValue(entry, kSecImportItemIdentity)));
        if (!identity) throw Error(ErrorCode::InvalidSelection, "The certificate file has no private key.");
    }
    ~Identity() { cleanup(); }
    void cleanup() {
        if (keychain) SecKeychainDelete(keychain.value);
        { Ref<SecKeychainRef> gone(std::move(keychain)); }
        std::error_code ec; if (!path.empty()) std::filesystem::remove_all(std::filesystem::path(path).parent_path(), ec);
        path.clear();
#pragma clang diagnostic pop
    }
};

Bytes cmsSign(const std::filesystem::path& p12, const std::string& password, const Bytes& signedContent) {
    Identity id(p12, password);
    CMSEncoderRef raw{};
    if (CMSEncoderCreate(&raw) != errSecSuccess) throw Error(ErrorCode::SaveFailed, "Signing is unavailable.");
    Ref<CMSEncoderRef> encoder(raw);
    CMSEncoderAddSigners(raw, id.identity);
    CMSEncoderSetHasDetachedContent(raw, true);
    CMSEncoderSetSignerAlgorithm(raw, kCMSEncoderDigestAlgorithmSHA256);
    CMSEncoderSetCertificateChainMode(raw, kCMSCertificateChainWithRoot);
    CMSEncoderAddSignedAttributes(raw, kCMSAttrSigningTime);
    if (CMSEncoderUpdateContent(raw, signedContent.data(), signedContent.size()) != errSecSuccess) throw Error(ErrorCode::SaveFailed, "Signing failed.");
    CFDataRef out{};
    if (CMSEncoderCopyEncodedContent(raw, &out) != errSecSuccess || !out) throw Error(ErrorCode::SaveFailed, "Signing failed. The certificate may not allow signing.");
    Ref<CFDataRef> holder(out);
    return Bytes(CFDataGetBytePtr(out), CFDataGetBytePtr(out) + CFDataGetLength(out));
}
// 1 = signature intact, 0 = content or signature altered, -1 = could not be evaluated.
int cmsVerify(const Bytes& cms, const Bytes& content, std::string& signer) {
    CMSDecoderRef raw{};
    if (CMSDecoderCreate(&raw) != errSecSuccess) return -1;
    Ref<CMSDecoderRef> decoder(raw);
    if (CMSDecoderUpdateMessage(raw, cms.data(), cms.size()) != errSecSuccess) return 0;
    auto detached = dataOf(content.data(), content.size());
    if (CMSDecoderSetDetachedContent(raw, detached.value) != errSecSuccess) return -1;
    if (CMSDecoderFinalizeMessage(raw) != errSecSuccess) return 0;
    Ref<SecPolicyRef> policy(SecPolicyCreateBasicX509());
    CMSSignerStatus status{}; Ref<SecTrustRef> trust; OSStatus verify{};
    SecTrustRef t{};
    if (CMSDecoderCopySignerStatus(raw, 0, policy.value, false, &status, &t, &verify) != errSecSuccess) return -1;
    trust = Ref<SecTrustRef>(t);
    CFStringRef name{};
    SecCertificateRef cert{};
    if (CMSDecoderCopySignerCert(raw, 0, &cert) == errSecSuccess && cert) {
        Ref<SecCertificateRef> holder(cert);
        if (SecCertificateCopyCommonName(cert, &name) == errSecSuccess && name) {
            Ref<CFStringRef> n(name); char buffer[256];
            if (CFStringGetCString(name, buffer, sizeof buffer, kCFStringEncodingUTF8)) signer = buffer;
        }
    }
    // An untrusted (self-signed) certificate still proves the content is unmodified.
    return status == kCMSSignerValid || status == kCMSSignerInvalidCert ? 1 : 0;
}
#else
Bytes cmsSign(const std::filesystem::path&, const std::string&, const Bytes&) { throw Error(ErrorCode::Unsupported, "Digital signing is currently available on macOS only."); }
int cmsVerify(const Bytes&, const Bytes&, std::string&) { return -1; }
#endif

// End offset of the BER element starting at pos, or 0 when malformed.
std::size_t berEnd(const Bytes& d, std::size_t pos, int depth) {
    if (depth > 32 || pos + 2 > d.size()) return 0;
    std::size_t at = pos + 1;
    if ((d[pos] & 0x1F) == 0x1F) { while (at < d.size() && d[at] & 0x80) ++at; ++at; }
    if (at >= d.size()) return 0;
    const unsigned char first = d[at++];
    if (first == 0x80) {
        while (true) {
            if (at + 2 <= d.size() && d[at] == 0 && d[at + 1] == 0) return at + 2;
            at = berEnd(d, at, depth + 1); if (at == 0) return 0;
        }
    }
    std::size_t length = first;
    if (first & 0x80) {
        const std::size_t n = first & 0x7F; if (n > 4 || at + n > d.size()) return 0;
        length = 0; for (std::size_t k = 0; k < n; ++k) length = length << 8 | d[at++];
    }
    return at + length <= d.size() ? at + length : 0;
}
std::size_t lastStartxref(const Bytes& pdf) {
    const std::string tail(pdf.end() - std::min<std::size_t>(pdf.size(), 2048), pdf.end());
    auto at = tail.rfind("startxref");
    if (at == std::string::npos) throw Error(ErrorCode::InvalidDocument, "The PDF has no cross-reference table.");
    return static_cast<std::size_t>(std::stoull(tail.substr(at + 9)));
}
}

Bytes sign(const Bytes& input, const SignOptions& options) {
    // A plain (non-compressed) cross-reference table makes the appended update universally readable.
    QPDF pdf; pdf.setSuppressWarnings(true);
    pdf.processMemoryFile("document", reinterpret_cast<const char*>(input.data()), input.size());
    Bytes base;
    {
        QPDFWriter writer(pdf); writer.setOutputMemory(); writer.setObjectStreamMode(qpdf_o_disable); writer.setPreserveEncryption(false); writer.write();
        auto buffer = writer.getBufferSharedPointer(); base.assign(buffer->getBuffer(), buffer->getBuffer() + buffer->getSize());
    }
    QPDF doc; doc.setSuppressWarnings(true);
    doc.processMemoryFile("document", reinterpret_cast<const char*>(base.data()), base.size());
    auto pages = QPDFPageDocumentHelper(doc).getAllPages();
    if (pages.empty()) throw Error(ErrorCode::InvalidDocument, "The document has no pages.");
    auto root = doc.getRoot(); auto trailer = doc.getTrailer();
    const std::size_t size = static_cast<std::size_t>(trailer.getKey("/Size").getUIntValue());
    const std::size_t previous = lastStartxref(base);

    auto page = pages.front().getObjectHandle();
    auto signatureObject = doc.makeIndirectObject(Obj::newNull());
    auto widget = Obj::newDictionary();
    widget.replaceKey("/Type", Obj::newName("/Annot")); widget.replaceKey("/Subtype", Obj::newName("/Widget"));
    widget.replaceKey("/FT", Obj::newName("/Sig"));
    std::size_t fieldCount = 0;
    auto form = root.getKey("/AcroForm");
    if (form.isDictionary() && form.getKey("/Fields").isArray()) fieldCount = static_cast<std::size_t>(form.getKey("/Fields").getArrayNItems());
    widget.replaceKey("/T", Obj::newUnicodeString("Signature" + std::to_string(fieldCount + 1)));
    widget.replaceKey("/Rect", Obj::newArray(Obj::Rectangle(0, 0, 0, 0)));
    widget.replaceKey("/F", Obj::newInteger(132));
    widget.replaceKey("/P", page); widget.replaceKey("/V", signatureObject);
    auto widgetObject = doc.makeIndirectObject(widget);

    std::vector<Obj> annots;
    if (page.getKey("/Annots").isArray()) annots = page.getKey("/Annots").getArrayAsVector();
    annots.push_back(widgetObject);
    page.replaceKey("/Annots", Obj::newArray(annots));
    std::vector<Obj> fields;
    Obj acro = Obj::newDictionary();
    if (form.isDictionary()) { acro = form.shallowCopy(); if (form.getKey("/Fields").isArray()) fields = form.getKey("/Fields").getArrayAsVector(); }
    fields.push_back(widgetObject);
    acro.replaceKey("/Fields", Obj::newArray(fields)); acro.replaceKey("/SigFlags", Obj::newInteger(3));
    root.replaceKey("/AcroForm", acro);

    Bytes out = base; out.push_back('\n');
    std::map<int, std::size_t> offsets;
    auto append = [&](const std::string& text) { out.insert(out.end(), text.begin(), text.end()); };
    auto writeObject = [&](Obj object, const std::string& body) {
        offsets[object.getObjectID()] = out.size();
        append(std::to_string(object.getObjectID()) + " 0 obj\n" + body + "\nendobj\n");
    };
    writeObject(page, unparsed(page));
    writeObject(root, unparsed(root));
    writeObject(widgetObject, unparsed(widgetObject));

    const std::string reason = options.reason.empty() ? std::string() : " /Reason " + Obj::newUnicodeString(options.reason).unparse();
    const std::string location = options.location.empty() ? std::string() : " /Location " + Obj::newUnicodeString(options.location).unparse();
    const std::string name = options.signerName.empty() ? std::string() : " /Name " + Obj::newUnicodeString(options.signerName).unparse();
    const std::string head = "<< /Type /Sig /Filter /Adobe.PPKLite /SubFilter /adbe.pkcs7.detached /ByteRange [0 " + pad10(0) + " " + pad10(0) + " " + pad10(0) + "] /Contents <";
    offsets[signatureObject.getObjectID()] = out.size();
    append(std::to_string(signatureObject.getObjectID()) + " 0 obj\n" + head);
    const std::size_t contentsStart = out.size() - 1; // The '<'.
    const std::size_t byteRangeAt = out.size() - head.size() + head.find("[0 ");
    append(std::string(signatureCapacity * 2, '0') + "> /M " + Obj::newString(pdfDate()).unparse() + reason + location + name + " >>\nendobj\n");
    const std::size_t contentsEnd = contentsStart + 1 + signatureCapacity * 2 + 1;

    const std::size_t xref = out.size();
    std::string table = "xref\n";
    for (auto it = offsets.begin(); it != offsets.end();) {
        auto run = it; std::size_t count = 0;
        for (auto next = it; next != offsets.end() && next->first == it->first + static_cast<int>(count); ++next) { ++count; run = next; }
        table += std::to_string(it->first) + " " + std::to_string(count) + "\n";
        for (auto walk = it; walk != std::next(run); ++walk) { char line[24]; std::snprintf(line, sizeof line, "%010zu 00000 n \n", walk->second); table += line; }
        it = std::next(run);
    }
    table += "trailer\n<< /Size " + std::to_string(size + 2) + " /Root " + std::to_string(root.getObjectID()) + " 0 R /Prev " + std::to_string(previous);
    if (trailer.hasKey("/Info")) table += " /Info " + trailer.getKey("/Info").unparse();
    if (trailer.hasKey("/ID")) table += " /ID " + trailer.getKey("/ID").unparseResolved();
    table += " >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    append(table);

    const std::string range = "[0 " + pad10(contentsStart) + " " + pad10(contentsEnd) + " " + pad10(out.size() - contentsEnd) + "]";
    std::copy(range.begin(), range.end(), out.begin() + static_cast<std::ptrdiff_t>(byteRangeAt));
    Bytes content(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(contentsStart));
    content.insert(content.end(), out.begin() + static_cast<std::ptrdiff_t>(contentsEnd), out.end());
    auto cms = cmsSign(options.certificate, options.password, content);
    if (cms.size() > signatureCapacity) throw Error(ErrorCode::ResourceLimit, "The certificate chain is too large to embed.");
    auto hex = hexOf(cms);
    std::copy(hex.begin(), hex.end(), out.begin() + static_cast<std::ptrdiff_t>(contentsStart + 1));
    return out;
}

std::vector<SignatureCheck> verify(const Bytes& pdf) {
    std::vector<SignatureCheck> results;
    const std::string text(pdf.begin(), pdf.end());
    for (std::size_t at = text.find("/ByteRange"); at != std::string::npos; at = text.find("/ByteRange", at + 1)) {
        SignatureCheck check;
        std::size_t r[4]{};
        if (std::sscanf(text.c_str() + at, "/ByteRange [%zu %zu %zu %zu]", &r[0], &r[1], &r[2], &r[3]) != 4) continue;
        if (r[0] != 0 || r[1] > pdf.size() || r[2] > pdf.size() || r[2] + r[3] > pdf.size() || r[1] > r[2]) { results.push_back(check); continue; }
        check.coversWholeFile = r[2] + r[3] == pdf.size();
        auto hexStart = text.find('<', r[1]); auto hexEnd = text.find('>', r[1]);
        if (hexStart != r[1] || hexEnd == std::string::npos) { results.push_back(check); continue; }
        Bytes cms;
        for (std::size_t i = hexStart + 1; i + 1 < hexEnd; i += 2) cms.push_back(static_cast<unsigned char>(std::stoi(text.substr(i, 2), nullptr, 16)));
        // The blob is zero-padded (and Apple emits indefinite-length BER), so walk the structure to find its real end.
        const std::size_t total = berEnd(cms, 0, 0);
        if (total == 0 || total > cms.size()) { results.push_back(check); continue; }
        cms.resize(total);
        Bytes content(pdf.begin(), pdf.begin() + static_cast<std::ptrdiff_t>(r[1]));
        content.insert(content.end(), pdf.begin() + static_cast<std::ptrdiff_t>(r[2]), pdf.begin() + static_cast<std::ptrdiff_t>(r[2] + r[3]));
        auto status = cmsVerify(cms, content, check.signer);
        check.intact = status == 1;
        results.push_back(check);
    }
    return results;
}
}
