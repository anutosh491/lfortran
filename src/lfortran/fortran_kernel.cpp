#include <iostream>
#include <cstdint>
#include <cstring>
#include <vector>

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#    include <io.h>
#    define fileno _fileno
#    define dup _dup
#    define dup2 _dup2
#    define close _close
#    include <fcntl.h>
#else
#    include <unistd.h>
#endif

#include <xeus/xinterpreter.hpp>
#include <xeus/xkernel.hpp>
#include <xeus/xkernel_configuration.hpp>
#include <xeus/xhelper.hpp>
#include <xeus/xbase64.hpp>
#ifdef __EMSCRIPTEN__
#include <emscripten/bind.h>
#include <xeus/xembind.hpp>
#else
#include <xeus-zmq/xzmq_context.hpp>
#include <xeus-zmq/xserver_zmq.hpp>
#endif

#include <nlohmann/json.hpp>

#include <lfortran/fortran_kernel.h>
#include <lfortran/parser/parser.h>
#include <lfortran/semantics/ast_to_asr.h>
#include <libasr/codegen/asr_to_llvm.h>
#include <lfortran/fortran_evaluator.h>
#include <libasr/asr_utils.h>
#include <libasr/string_utils.h>

namespace nl = nlohmann;

// ── Jupyter display_data bridge ──────────────────────────────────────────────
// These C-linkage symbols are called from JIT'd Fortran code via bind(C)
// declarations injected at REPL startup in configure_impl().
// Native (ORC JIT): resolved from the host process automatically.
// WASM: defined in the MAIN_MODULE; side modules import via --allow-undefined
// and RTLD_DEFAULT resolves them at dlopen time.

extern "C" {

void lfortran_display_data(const char* mime_type, const char* data) {
    if (!mime_type || !data) return;
    nl::json bundle = nl::json::object();
    bundle[mime_type] = std::string(data);
    xeus::get_interpreter().display_data(
        std::move(bundle), nl::json::object(), nl::json::object());
}

// Encode a Fortran column-major RGBA pixel array A(4,w,h) as a 24-bit BMP,
// base64-encode it, and publish as an image/bmp display_data message.
// In memory: channel varies fastest (stride 1), then column (stride 4),
// then row (stride 4*w).  Channels: 0=R, 1=G, 2=B, 3=A.
void lfortran_display_image_rgba(int w, int h, int* data) {
    // BMP rows are 24-bit BGR, bottom-up, padded to 4-byte alignment.
    int row_stride = ((w * 3 + 3) / 4) * 4;
    int img_bytes  = row_stride * h;
    int file_size  = 54 + img_bytes;

    std::vector<uint8_t> bmp(file_size, 0);
    uint8_t* p = bmp.data();

    // File header (14 bytes)
    p[0] = 'B'; p[1] = 'M';
    p[2] = file_size & 0xFF; p[3] = (file_size >> 8) & 0xFF;
    p[4] = (file_size >> 16) & 0xFF; p[5] = (file_size >> 24) & 0xFF;
    p[10] = 54; // pixel data offset

    // BITMAPINFOHEADER (40 bytes starting at byte 14)
    p[14] = 40;
    p[18] = w & 0xFF; p[19] = (w >> 8) & 0xFF;
    p[20] = (w >> 16) & 0xFF; p[21] = (w >> 24) & 0xFF;
    p[22] = h & 0xFF; p[23] = (h >> 8) & 0xFF;
    p[24] = (h >> 16) & 0xFF; p[25] = (h >> 24) & 0xFF;
    p[26] = 1;   // color planes
    p[28] = 24;  // bits per pixel (24-bit BGR, no alpha)

    // Pixel data: BMP row 0 is the bottom of the image.
    // Fortran row 0 (j=1) is the top → map BMP row r to Fortran row h-1-r.
    uint8_t* pixels = p + 54;
    for (int r = 0; r < h; r++) {
        int fort_row = h - 1 - r;
        uint8_t* row_ptr = pixels + r * row_stride;
        for (int c = 0; c < w; c++) {
            int idx = 4 * c + 4 * w * fort_row;
            row_ptr[c * 3 + 0] = static_cast<uint8_t>(data[idx + 2]); // B
            row_ptr[c * 3 + 1] = static_cast<uint8_t>(data[idx + 1]); // G
            row_ptr[c * 3 + 2] = static_cast<uint8_t>(data[idx + 0]); // R
        }
    }

    std::string raw(reinterpret_cast<char*>(bmp.data()), bmp.size());
    std::string b64 = xeus::base64encode(raw);
    lfortran_display_data("image/bmp", b64.c_str());
}

} // extern "C"

// ── Display bootstrap module ──────────────────────────────────────────────────
// Evaluated once in configure_impl() before any user cell.  After this runs,
// all subsequent cells can `use lfortran_display` and call display_html(),
// display_image(), display_svg(), display_latex(), display_markdown().

static constexpr const char* kDisplaySetupCode = R"fortran(
module lfortran_display
  use iso_c_binding, only: c_char, c_int, c_null_char
  implicit none

  interface
    subroutine lf_display_data(mime, payload) bind(C, name="lfortran_display_data")
      import :: c_char
      character(kind=c_char), intent(in) :: mime(*), payload(*)
    end subroutine

    subroutine lf_display_image_rgba(w, h, rgba) &
        bind(C, name="lfortran_display_image_rgba")
      import :: c_int
      integer(c_int), value       :: w, h
      integer(c_int), intent(in)  :: rgba(*)
    end subroutine
  end interface

contains

  subroutine display_html(html)
    character(len=*), intent(in) :: html
    call lf_display_data("text/html"//c_null_char, trim(html)//c_null_char)
  end subroutine

  subroutine display_svg(svg)
    character(len=*), intent(in) :: svg
    call lf_display_data("image/svg+xml"//c_null_char, trim(svg)//c_null_char)
  end subroutine

  subroutine display_latex(s)
    character(len=*), intent(in) :: s
    call lf_display_data("text/latex"//c_null_char, trim(s)//c_null_char)
  end subroutine

  subroutine display_markdown(s)
    character(len=*), intent(in) :: s
    call lf_display_data("text/markdown"//c_null_char, trim(s)//c_null_char)
  end subroutine

  subroutine display_image(w, h, rgba)
    integer(c_int), intent(in) :: w, h
    integer(c_int), intent(in) :: rgba(4, w, h)
    call lf_display_image_rgba(w, h, rgba)
  end subroutine

end module lfortran_display
)fortran";

namespace LCompilers::LFortran {


    class RedirectStdout
    {
    public:
        RedirectStdout(std::string &out) : _out{out} {
            stdout_fileno = fileno(stdout);
            std::cout << std::flush;
            fflush(stdout);
            saved_stdout = dup(stdout_fileno);
#ifdef _WIN32
            if (_pipe(out_pipe, 65536, O_BINARY) != 0) {
#else
            if (pipe(out_pipe) != 0) {
#endif
                throw LCompilersException("pipe() failed");
            }
            dup2(out_pipe[1], stdout_fileno);
            close(out_pipe[1]);
            printf("X");
        }

        ~RedirectStdout() {
            fflush(stdout);
            read(out_pipe[0], buffer, MAX_LEN);
            dup2(saved_stdout, stdout_fileno);
            _out = std::string(&buffer[1]);
        }
    private:
        std::string &_out;
        static const size_t MAX_LEN=1024;
        char buffer[MAX_LEN+1] = {0};
        int out_pipe[2];
        int saved_stdout;
        int stdout_fileno;
    };

    class custom_interpreter : public xeus::xinterpreter
    {
    private:
        CompilerOptions compiler_options;
        FortranEvaluator e;

    public:
        custom_interpreter() : compiler_options{}, e{compiler_options} {
            e.compiler_options.interactive = true;
            e.compiler_options.po.runtime_library_dir =
                LCompilers::LFortran::get_runtime_library_dir();
            std::cerr << "[xlfortran] runtime_library_dir = "
                      << e.compiler_options.po.runtime_library_dir << std::endl;
        }
        virtual ~custom_interpreter() = default;

    private:

        void configure_impl() override;

        void execute_request_impl(send_reply_callback cb,
                                  int execution_counter,
                                  const std::string& code,
                                  //bool silent,
                                  //bool store_history,
                                  xeus::execute_request_config config,
                                  nl::json user_expressions) override;

        nl::json complete_request_impl(const std::string& code,
                                       int cursor_pos) override;

        nl::json inspect_request_impl(const std::string& code,
                                      int cursor_pos,
                                      int detail_level) override;

        nl::json is_complete_request_impl(const std::string& code) override;

        nl::json kernel_info_request_impl() override;

        nl::json shutdown_request_impl(bool restart) override;
        nl::json interrupt_request_impl() override;
    };

    
    void custom_interpreter::execute_request_impl(send_reply_callback cb,
                                                  int execution_counter, // Typically the cell number
                                                  const std::string& code, // Code to execute
                                                  xeus::execute_request_config, //config
                                                  nl::json /*user_expressions*/)
    {
        FortranEvaluator::EvalResult r;
        std::string std_out;
        std::string code0;
        CompilerOptions cu;
        try {
            if (startswith(code, "%%showast")) {
                code0 = code.substr(code.find("\n")+1);
                LocationManager lm;
                {
                    LocationManager::FileLocations fl;
                    fl.in_filename = "input";
                    std::ofstream out("input");
                    out << code0;
                    lm.files.push_back(fl);
                }
                diag::Diagnostics diagnostics;
                Result<std::string>
                    res = e.get_ast(code0, lm, diagnostics);
                nl::json result;
                if (res.ok) {
                    publish_stream("stdout", res.result);
                    result["status"] = "ok";
                    result["payload"] = nl::json::array();
                    result["user_expressions"] = nl::json::object();
                } else {
                    std::string msg = diagnostics.render(lm, cu);
                    publish_stream("stderr", msg);
                    result["status"] = "error";
                    result["ename"] = "CompilerError";
                    result["evalue"] = msg;
                    result["traceback"] = nl::json::array();
                }
                cb(result);
                return;
            }
            if (startswith(code, "%%showasr")) {
                code0 = code.substr(code.find("\n")+1);
                LocationManager lm;
                {
                    LocationManager::FileLocations fl;
                    fl.in_filename = "input";
                    std::ofstream out("input");
                    out << code0;
                    lm.files.push_back(fl);
                }
                diag::Diagnostics diagnostics;
                Result<std::string>
                res = e.get_asr(code0, lm, diagnostics);
                nl::json result;
                if (res.ok) {
                    publish_stream("stdout", res.result);
                    result["status"] = "ok";
                    result["payload"] = nl::json::array();
                    result["user_expressions"] = nl::json::object();
                } else {
                    std::string msg = diagnostics.render(lm, cu);
                    publish_stream("stderr", msg);
                    result["status"] = "error";
                    result["ename"] = "CompilerError";
                    result["evalue"] = msg;
                    result["traceback"] = nl::json::array();
                }
                cb(result);
                return;
            }
            if (startswith(code, "%%showllvm")) {
                code0 = code.substr(code.find("\n")+1);
                LocationManager lm;
                {
                    LocationManager::FileLocations fl;
                    fl.in_filename = "input";
                    std::ofstream out("input");
                    out << code0;
                    lm.files.push_back(fl);
                }
                LCompilers::PassManager lpm;
                lpm.use_default_passes();
                diag::Diagnostics diagnostics;
                Result<std::string>
                res = e.get_llvm(code0, lm, lpm, diagnostics);
                nl::json result;
                if (res.ok) {
                    publish_stream("stdout", res.result);
                    result["status"] = "ok";
                    result["payload"] = nl::json::array();
                    result["user_expressions"] = nl::json::object();
                } else {
                    std::string msg = diagnostics.render(lm, cu);
                    publish_stream("stderr", msg);
                    result["status"] = "error";
                    result["ename"] = "CompilerError";
                    result["evalue"] = msg;
                    result["traceback"] = nl::json::array();
                }
                cb(result);
                return;
            }
            if (startswith(code, "%%showasm")) {
                code0 = code.substr(code.find("\n")+1);
                LocationManager lm;
                {
                    LocationManager::FileLocations fl;
                    fl.in_filename = "input";
                    std::ofstream out("input");
                    out << code0;
                    lm.files.push_back(fl);
                }
                LCompilers::PassManager lpm;
                lpm.use_default_passes();
                diag::Diagnostics diagnostics;
                Result<std::string>
                res = e.get_asm(code0, lm, lpm, diagnostics);
                nl::json result;
                if (res.ok) {
                    publish_stream("stdout", res.result);
                    result["status"] = "ok";
                    result["payload"] = nl::json::array();
                    result["user_expressions"] = nl::json::object();
                } else {
                    std::string msg = diagnostics.render(lm, cu);
                    publish_stream("stderr", msg);
                    result["status"] = "error";
                    result["ename"] = "CompilerError";
                    result["evalue"] = msg;
                    result["traceback"] = nl::json::array();
                }
                cb(result);
                return;
            }
            if (startswith(code, "%%showcpp")) {
                code0 = code.substr(code.find("\n")+1);
                LocationManager lm;
                {
                    LocationManager::FileLocations fl;
                    fl.in_filename = "input";
                    std::ofstream out("input");
                    out << code0;
                    lm.files.push_back(fl);
                }
                diag::Diagnostics diagnostics;
                Result<std::string>
                res = e.get_cpp(code0, lm, diagnostics, 1);
                nl::json result;
                if (res.ok) {
                    publish_stream("stdout", res.result);
                    result["status"] = "ok";
                    result["payload"] = nl::json::array();
                    result["user_expressions"] = nl::json::object();
                } else {
                    std::string msg = diagnostics.render(lm, cu);
                    publish_stream("stderr", msg);
                    result["status"] = "error";
                    result["ename"] = "CompilerError";
                    result["evalue"] = msg;
                    result["traceback"] = nl::json::array();
                }
                cb(result);
                return;
            }
            if (startswith(code, "%%showfmt")) {
                code0 = code.substr(code.find("\n")+1);
                LocationManager lm;
                {
                    LocationManager::FileLocations fl;
                    fl.in_filename = "input";
                    std::ofstream out("input");
                    out << code0;
                    lm.files.push_back(fl);
                }
                diag::Diagnostics diagnostics;
                Result<std::string>
                res = e.get_fmt(code0, lm, diagnostics);
                nl::json result;
                if (res.ok) {
                    publish_stream("stdout", res.result);
                    result["status"] = "ok";
                    result["payload"] = nl::json::array();
                    result["user_expressions"] = nl::json::object();
                } else {
                    std::string msg = diagnostics.render(lm, cu);
                    publish_stream("stderr", msg);
                    result["status"] = "error";
                    result["ename"] = "CompilerError";
                    result["evalue"] = msg;
                    result["traceback"] = nl::json::array();
                }
                cb(result);
                return;
            }

            RedirectStdout s(std_out);
            code0 = code;
            LocationManager lm;
            {
                LocationManager::FileLocations fl;
                fl.in_filename = "input";
                std::ofstream out("input");
                out << code0;
                lm.files.push_back(fl);
            }
            LCompilers::PassManager lpm;
            lpm.use_default_passes();
            diag::Diagnostics diagnostics;
            Result<FortranEvaluator::EvalResult>
            res = e.evaluate(code0, false, lm, lpm, diagnostics);
            if (res.ok) {
                r = res.result;
            } else {
                std::string msg = diagnostics.render(lm, cu);
                publish_stream("stderr", msg);
                nl::json result;
                result["status"] = "error";
                result["ename"] = "CompilerError";
                result["evalue"] = msg;
                result["traceback"] = nl::json::array();
                cb(result);
                return;
            }
        } catch (const LCompilersException &e) {
            publish_stream("stderr", "LFortran Exception: " + e.msg());
            nl::json result;
            result["status"] = "error";
            result["ename"] = "LCompilersException";
            result["evalue"] = e.msg();
            result["traceback"] = nl::json::array();
            cb(result);
            return;
        }

        if (std_out.size() > 0) {
            publish_stream("stdout", std_out);
        }

        switch (r.type) {
            case (LCompilers::FortranEvaluator::EvalResult::integer4) : {
                nl::json pub_data;
                pub_data["text/plain"] = std::to_string(r.i32);
                publish_execution_result(execution_counter, std::move(pub_data), nl::json::object());
                break;
            }
            case (LCompilers::FortranEvaluator::EvalResult::integer8) : {
                nl::json pub_data;
                pub_data["text/plain"] = std::to_string(r.i64);
                publish_execution_result(execution_counter, std::move(pub_data), nl::json::object());
                break;
            }
            case (LCompilers::FortranEvaluator::EvalResult::real4) : {
                nl::json pub_data;
                pub_data["text/plain"] = std::to_string(r.f32);
                publish_execution_result(execution_counter, std::move(pub_data), nl::json::object());
                break;
            }
            case (LCompilers::FortranEvaluator::EvalResult::real8) : {
                nl::json pub_data;
                pub_data["text/plain"] = std::to_string(r.f64);
                publish_execution_result(execution_counter, std::move(pub_data), nl::json::object());
                break;
            }
            case (LCompilers::FortranEvaluator::EvalResult::complex4) : {
                nl::json pub_data;
                pub_data["text/plain"] = "(" + std::to_string(r.c32.re) + ", " + std::to_string(r.c32.im) + ")";
                publish_execution_result(execution_counter, std::move(pub_data), nl::json::object());
                break;
            }
            case (LCompilers::FortranEvaluator::EvalResult::complex8) : {
                nl::json pub_data;
                pub_data["text/plain"] = "(" + std::to_string(r.c64.re) + ", " + std::to_string(r.c64.im) + ")";
                publish_execution_result(execution_counter, std::move(pub_data), nl::json::object());
                break;
            }
            case (LCompilers::FortranEvaluator::EvalResult::statement) : {
                break;
            }
            case (LCompilers::FortranEvaluator::EvalResult::none) : {
                break;
            }
            default : throw LCompilersException("Return type not supported");
        }

        nl::json result;
        result["status"] = "ok";
        result["payload"] = nl::json::array();
        result["user_expressions"] = nl::json::object();
        cb(result);
        return;
    }
    
    void custom_interpreter::configure_impl()
    {
        xeus::register_interpreter(this);

        // Inject the lfortran_display module before the first user cell so that
        // display_html(), display_image(), display_svg() etc. are available in
        // every subsequent cell — the same pattern xeus-swift uses for its
        // kDisplaySetupCode / MimeBundleRepresentable bootstrap.
        LocationManager lm;
        {
            LocationManager::FileLocations fl;
            fl.in_filename = "input";
            std::ofstream out("input");
            out << kDisplaySetupCode;
            lm.files.push_back(fl);
        }
        LCompilers::PassManager lpm;
        lpm.use_default_passes();
        diag::Diagnostics diagnostics;
        CompilerOptions cu;
        auto res = e.evaluate(kDisplaySetupCode, false, lm, lpm, diagnostics);
        if (!res.ok) {
            std::string msg = diagnostics.render(lm, cu);
            std::cerr << "[xlfortran] Warning: display setup failed: " << msg << "\n";
        }
    }

    nl::json custom_interpreter::complete_request_impl(const std::string& code,
                                                       int cursor_pos)
    {
        nl::json result;

        // Code starts with 'H', it could be the following completion
        if (code[0] == 'H')
        {
            result["status"] = "ok";
            result["matches"] = {"Hello", "Hey", "Howdy"};
            result["cursor_start"] = 5;
            result["cursor_end"] = cursor_pos;
        }
        // No completion result
        else
        {
            result["status"] = "ok";
            result["matches"] = nl::json::array();
            result["cursor_start"] = cursor_pos;
            result["cursor_end"] = cursor_pos;
        }

        return result;
    }

    nl::json custom_interpreter::inspect_request_impl(const std::string& code,
                                                      int /*cursor_pos*/,
                                                      int /*detail_level*/)
    {
        nl::json result;

        if (code.compare("print") == 0)
        {
            result["found"] = true;
            result["text/plain"] = "Print objects to the text stream file, [...]";
        }
        else
        {
            result["found"] = false;
        }

        result["status"] = "ok";
        return result;
    }

    nl::json custom_interpreter::is_complete_request_impl(const std::string& /*code*/)
    {
        nl::json result;

        // if (is_complete(code))
        // {
            result["status"] = "complete";
        // }
        // else
        // {
        //    result["status"] = "incomplete";
        //    result["indent"] = 4;
        //}

        return result;
    }

    nl::json custom_interpreter::kernel_info_request_impl()
    {
        nl::json result;
        std::string version = LFORTRAN_VERSION;
        std::string banner = ""
            "LFortran " + version + "\n"
            "Jupyter kernel for Fortran";
        result["banner"] = banner;
        result["implementation"] = "LFortran";
        result["implementation_version"] = version;
        result["language_info"]["name"] = "fortran";
        result["language_info"]["version"] = "2018";
        result["language_info"]["mimetype"] = "text/x-fortran";
        result["language_info"]["file_extension"] = ".f90";
        return result;
    }

    nl::json custom_interpreter::shutdown_request_impl(bool restart) {
        std::cout << "Bye!!" << std::endl;
        return xeus::create_shutdown_reply(restart);
    }

    nl::json custom_interpreter::interrupt_request_impl() {
        return xeus::create_interrupt_reply();
    }

    int run_kernel(const std::string &connection_filename)
    {
#ifdef __EMSCRIPTEN__
        (void)connection_filename;
        throw LCompilersException("run_kernel() is not available in the WASM build");
        return 1;
#else
        std::unique_ptr<xeus::xcontext> context = xeus::make_zmq_context();

        // Create interpreter instance
        using interpreter_ptr = std::unique_ptr<custom_interpreter>;
        interpreter_ptr interpreter = interpreter_ptr(new custom_interpreter());

        using history_manager_ptr = std::unique_ptr<xeus::xhistory_manager>;
        history_manager_ptr hist = xeus::make_in_memory_history_manager();

        nl::json debugger_config;

        // Load configuration file
        xeus::xconfiguration config = xeus::load_configuration(connection_filename);

        // Create kernel instance and start it
        xeus::xkernel kernel(config,
                             xeus::get_user_name(),
                             std::move(context),
                             std::move(interpreter),
                             xeus::make_xserver_default,
                             std::move(hist),
                             xeus::make_console_logger(xeus::xlogger::msg_type,
                                                       xeus::make_file_logger(xeus::xlogger::content, "xeus.log")),
                             xeus::make_null_debugger,
                             debugger_config);

        std::cout <<
            "Starting xeus-fortran kernel...\n\n"
            "If you want to connect to this kernel from an other client, you can use"
            " the " + connection_filename + " file."
            << std::endl;

        kernel.start();

        return 0;
#endif // __EMSCRIPTEN__
    }

} // namespace LCompilers::LFortran

#ifdef __EMSCRIPTEN__
namespace {
    LCompilers::LFortran::custom_interpreter* make_interpreter(emscripten::val /*js_args*/)
    {
        return new LCompilers::LFortran::custom_interpreter();
    }
}

EMSCRIPTEN_BINDINGS(my_module)
{
    xeus::export_core();
    xeus::export_kernel<LCompilers::LFortran::custom_interpreter,
                        &make_interpreter>("xkernel");
}
#endif // __EMSCRIPTEN__
