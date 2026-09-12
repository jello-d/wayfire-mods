// workspace-wallpaper.cpp
//
// Per-output Wayfire plugin painting a per-workspace wallpaper into the
// BACKGROUND layer. With multiple outputs it spans one logical image across
// the whole output layout (each output draws its slice); with a single output
// it covers that output. The same code path serves the laptop and the
// multi-monitor desktop, so there is no "multiple monitors" config branch.
//
// Spanning correctness: the wallpaper is cover-scaled ONCE against the whole
// output-layout union, producing a single rectangle in global (union) coords.
// Every output draws that identical rectangle, shifted into its own local
// frame by -origin, and the scissor clips it to that output. Because the
// rectangle is computed from the union (not per-output), the slices tile
// exactly -- a feature crossing a bezel is continuous. Per-output centering is
// never used for the wallpaper, which is what previously broke the seams.

#include <wayfire/plugin.hpp>
#include <wayfire/per-output-plugin.hpp>
#include <wayfire/output.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/scene.hpp>
#include <wayfire/scene-operations.hpp>
#include <wayfire/scene-render.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/workspace-set.hpp>
#include <wayfire/util/log.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// Image loading (JPEG/PNG/etc).
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

namespace fs = std::filesystem;

enum class fit_mode_t {
  FILL,     // cover
  FIT,      // contain
  STRETCH,
  CENTER,
};

struct gl_texture_t {
  GLuint tex = (GLuint)-1;
  int width = 0;
  int height = 0;
  bool valid = false;

  void destroy() {
    if (valid && (tex != (GLuint)-1)) {
      GLuint t = tex;
      wf::gles::run_in_context([&] { glDeleteTextures(1, &t); });
    }
    tex = (GLuint)-1;
    width = height = 0;
    valid = false;
  }
};

static GLuint create_solid_rgba_texture(uint8_t r, uint8_t g, uint8_t b,
                                        uint8_t a) {
  GLuint tex = (GLuint)-1;
  wf::gles::run_in_context([&] {
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    uint8_t px[4] = {r, g, b, a};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 px);
  });
  return tex;
}

static bool upload_rgba_to_gl(gl_texture_t& out, const uint8_t *rgba, int w,
                              int h) {
  if (!rgba || (w <= 0) || (h <= 0)) return false;

  out.destroy();
  out.width = w;
  out.height = h;

  wf::gles::run_in_context([&] {
    glGenTextures(1, &out.tex);
    glBindTexture(GL_TEXTURE_2D, out.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgba);
  });

  out.valid = true;
  return true;
}

static std::optional<fs::path> find_image_for_cell(const fs::path& dir,
                                                   const std::string& base) {
  static const char *exts[] = {".jpg", ".jpeg", ".png"};
  for (auto ext : exts) {
    fs::path p = dir / (base + ext);
    if (fs::exists(p)) return p;
  }
  return std::nullopt;
}

static fit_mode_t parse_mode(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), ::tolower);
  if ((s == "cover") || (s == "fill")) return fit_mode_t::FILL;
  if ((s == "contain") || (s == "fit")) return fit_mode_t::FIT;
  if (s == "stretch") return fit_mode_t::STRETCH;
  if (s == "center") return fit_mode_t::CENTER;
  return fit_mode_t::FILL;  // default: most wallpaper-y
}

// Cover/contain/stretch/center the image over a target rect of size
// (span_w x span_h), returning the image's destination rect in the SAME
// coordinate space as (span_x, span_y). This is the single source of fit math;
// it is called once per workspace cell against the GLOBAL union rect, so every
// output that shares a cell gets a byte-identical rectangle.
static inline wlr_box fit_image(fit_mode_t mode, int span_x, int span_y,
                                int span_w, int span_h, int img_w, int img_h) {
  if ((img_w <= 0) || (img_h <= 0)) return {span_x, span_y, span_w, span_h};
  if (mode == fit_mode_t::STRETCH) return {span_x, span_y, span_w, span_h};

  if (mode == fit_mode_t::CENTER) {
    int dx = span_x + (span_w - img_w) / 2;
    int dy = span_y + (span_h - img_h) / 2;
    return {dx, dy, img_w, img_h};
  }

  double sx = (double)span_w / (double)img_w;
  double sy = (double)span_h / (double)img_h;
  double s = (mode == fit_mode_t::FILL) ? std::max(sx, sy) : std::min(sx, sy);

  int dw = (int)std::round(img_w * s);
  int dh = (int)std::round(img_h * s);
  int dx = span_x + (span_w - dw) / 2;
  int dy = span_y + (span_h - dh) / 2;
  return {dx, dy, dw, dh};
}

namespace wf::scene::workspace_wallpaper {

class wallpaper_node_t;

class wallpaper_render_instance_t : public render_instance_t {
  wf::signal::connection_t<node_damage_signal> on_node_damaged =
      [=](node_damage_signal *ev) { push_to_parent(ev->region); };

  node_t *self;
  damage_callback push_to_parent;

  // Node geometry, in parent coords.
  int *x, *y, *w, *h;

  // Backpointer to the owning node (texture + mode + cover-rect access).
  wallpaper_node_t *owner;

 public:
  wallpaper_render_instance_t(node_t *self, damage_callback push_dmg, int *x,
                              int *y, int *w, int *h, wallpaper_node_t *owner) {
    this->x = x; this->y = y; this->w = w; this->h = h;
    this->self = self;
    this->owner = owner;
    this->push_to_parent = push_dmg;
    self->connect(&on_node_damaged);
  }

  void schedule_instructions(std::vector<render_instruction_t>& instructions,
                             const wf::render_target_t& target,
                             wf::regionf_t& damage) override {
    instructions.push_back(render_instruction_t{
        .instance = this,
        .target = target,
        .damage = damage & self->get_bounding_box(),
    });
  }

  void render(const wf::scene::render_instruction_t& data) override;
};

class wallpaper_node_t : public node_t {
  int x = 0, y = 0, w = 0, h = 0;

 public:
  // Set by the plugin after creation.
  gl_texture_t *wall_tex = nullptr;
  GLuint black_tex = (GLuint)-1;
  fit_mode_t mode = fit_mode_t::FILL;

  // Global cover rectangle for this cell: where the (cover-scaled) image sits,
  // in GLOBAL union coordinates, computed once against the whole layout. Every
  // output that paints this workspace cell stores the SAME cover_* values, so
  // the slices tile exactly. The render shifts this into the output's local
  // frame using out_origin_* (this output's global top-left). Defaults make a
  // single output behave like a plain per-output cover.
  int cover_x = 0, cover_y = 0, cover_w = 0, cover_h = 0;
  int out_origin_x = 0, out_origin_y = 0;

  wallpaper_node_t(int x, int y, int w, int h)
      : node_t(false), x(x), y(y), w(w), h(h),
        cover_x(0), cover_y(0), cover_w(w), cover_h(h),
        out_origin_x(0), out_origin_y(0) {}

  void gen_render_instances(std::vector<render_instance_uptr>& instances,
                            damage_callback push_damage,
                            wf::output_t *shown_on) override {
    instances.push_back(std::make_unique<wallpaper_render_instance_t>(
        this, push_damage, &x, &y, &w, &h, this));
  }

  wf::geometry_t get_bounding_box() override { return {x, y, w, h}; }

  void set_position(int nx, int ny) {
    x = nx; y = ny;
    emit_damage_whole();
  }

  void set_size(int nw, int nh) {
    w = nw; h = nh;
    emit_damage_whole();
  }

  // Cover rect (global union coords) + this output's global origin. The render
  // draws cover shifted by (node pos - out_origin); the scissor slices.
  void set_cover(int cx, int cy, int cw, int ch, int ox, int oy) {
    cover_x = cx; cover_y = cy; cover_w = cw; cover_h = ch;
    out_origin_x = ox; out_origin_y = oy;
    emit_damage_whole();
  }

  void emit_damage_whole() {
    node_damage_signal ev;
    // 0.11: node_damage_signal.region is regionf_t; build it from the node's
    // global box directly (regionf_t takes a geometry_t, not a wlr_box).
    ev.region = wf::regionf_t{wf::geometry_t{x, y, w, h}};
    this->emit(&ev);
  }

  friend class wallpaper_render_instance_t;
};

void wallpaper_render_instance_t::render(
    const wf::scene::render_instruction_t& data) {
  // 0.11: OpenGL::render_texture takes wf::geometry_t (float), not wlr_box.
  wf::geometry_t node_box = {*x, *y, *w, *h};

  data.pass->custom_gles_subpass([&] {
    wf::gles::bind_render_buffer(data.target);

    for (auto& box : data.damage) {
      // 0.11: damage boxes are pixman_box64f_t; the scissor takes one directly.
      wf::gles::render_target_logic_scissor(data.target, box);

      // 1) Black background everywhere (FIT letterboxing).
      if (owner->black_tex != (GLuint)-1) {
        OpenGL::render_texture(wf::gles_texture_t{owner->black_tex},
                               data.target, node_box, glm::vec4(1.0),
                               OpenGL::TEXTURE_TRANSFORM_INVERT_Y);
      }

      // 2) Wallpaper on top. cover_* is the image's rect in GLOBAL union
      //    coords (identical on every output for this cell). Translate it into
      //    this output's local render frame: subtract the output's global
      //    origin to land at the output's top-left, then add the node's own
      //    position so the image slides with the workspace. The scissor clips
      //    to the damaged box, so each output shows only its slice -- and
      //    because every output draws the same global rect, the slices tile
      //    seamlessly.
      if (owner->wall_tex && owner->wall_tex->valid) {
        auto& t = *owner->wall_tex;
        wf::geometry_t dst = {
            node_box.x + (owner->cover_x - owner->out_origin_x),
            node_box.y + (owner->cover_y - owner->out_origin_y),
            (double)owner->cover_w,
            (double)owner->cover_h,
        };

        OpenGL::render_texture(wf::gles_texture_t{t.tex}, data.target, dst,
                               glm::vec4(1.0),
                               OpenGL::TEXTURE_TRANSFORM_INVERT_Y);
      }
    }
  });
}

}  // namespace wf::scene::workspace_wallpaper

class wayfire_workspace_wallpaper : public wf::per_output_plugin_instance_t {
  wf::option_wrapper_t<std::string> directory{"workspace-wallpaper/directory"};
  wf::option_wrapper_t<std::string> pattern{"workspace-wallpaper/pattern"};
  wf::option_wrapper_t<std::string> mode_opt{"workspace-wallpaper/mode"};
  wf::option_wrapper_t<std::string> manifest{"workspace-wallpaper/manifest"};

  GLuint black_tex = (GLuint)-1;

  wf::dimensions_t grid_size{1, 1};
  wf::geometry_t og{0, 0, 0, 0};

  std::vector<std::vector<gl_texture_t>> textures;
  std::vector<std::vector<
      std::shared_ptr<wf::scene::workspace_wallpaper::wallpaper_node_t>>> nodes;

  fit_mode_t current_mode = fit_mode_t::FILL;
  std::map<std::pair<int, int>, std::string> manifest_map;

  static std::string expand_home(const std::string& in) {
    if (!in.empty() && (in[0] == '~') && (in.size() == 1 || in[1] == '/')) {
      const char *home = getenv("HOME");
      if (home) return std::string(home) + in.substr(1);
    }
    return in;
  }

  // Union of every output's layout geometry, in global coords. Built only from
  // outputs that are actually enabled with a non-empty rect, so a half-attached
  // layout during startup can't produce a partial/offset union. For a single
  // output this is just that output's own rect.
  wf::geometry_t layout_union() {
    bool have = false;
    // 0.11: geometry fields are double; keep the accumulators double so the
    // std::min/std::max against g.x/g.y type-match.
    double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    for (auto wo : wf::get_core().output_layout->get_outputs()) {
      wf::geometry_t g = wo->get_layout_geometry();
      if ((g.width <= 0) || (g.height <= 0)) continue;
      if (!have) {
        x1 = g.x; y1 = g.y; x2 = g.x + g.width; y2 = g.y + g.height;
        have = true;
      } else {
        x1 = std::min(x1, g.x);
        y1 = std::min(y1, g.y);
        x2 = std::max(x2, g.x + g.width);
        y2 = std::max(y2, g.y + g.height);
      }
    }
    if (!have) return output->get_layout_geometry();  // fallback: just us
    return {x1, y1, x2 - x1, y2 - y1};
  }

  void rebuild_nodes() {
    grid_size = output->wset()->get_workspace_grid_size();
    og = output->get_relative_geometry();

    nodes.clear();
    nodes.resize(grid_size.width);
    for (int x = 0; x < grid_size.width; x++) nodes[x].resize(grid_size.height);

    auto layer_root = output->node_for_layer(wf::scene::layer::BACKGROUND);

    for (int x = 0; x < grid_size.width; x++) {
      for (int y = 0; y < grid_size.height; y++) {
        auto n = std::make_shared<
            wf::scene::workspace_wallpaper::wallpaper_node_t>(
            x * og.width, y * og.height, og.width, og.height);

        n->black_tex = black_tex;
        n->wall_tex = &textures[x][y];
        n->mode = current_mode;

        wf::scene::add_front(layer_root, n);
        nodes[x][y] = n;
      }
    }

    apply_viewport(output->wset()->get_current_workspace());
  }

  std::string format_base_name(int x, int y) {
    std::string p = pattern;
    auto replace_all = [&](const std::string& what, const std::string& with) {
      size_t pos = 0;
      while ((pos = p.find(what, pos)) != std::string::npos) {
        p.replace(pos, what.size(), with);
        pos += with.size();
      }
    };
    replace_all("{x}", std::to_string(x));
    replace_all("{y}", std::to_string(y));
    return p;
  }

  void load_manifest() {
    manifest_map.clear();
    std::string path = expand_home((std::string)manifest);
    if (path.empty()) return;

    std::ifstream in(path);
    if (!in) {
      LOGW("workspace-wallpaper: manifest not found: ", path);
      return;
    }

    std::string line;
    while (std::getline(in, line)) {
      size_t a = line.find_first_not_of(" \t");
      if ((a == std::string::npos) || (line[a] == '#')) continue;

      std::istringstream ss(line.substr(a));
      int x, y;
      if (!(ss >> x >> y)) continue;

      std::string file;
      std::getline(ss, file);
      size_t b = file.find_first_not_of(" \t");
      size_t c = file.find_last_not_of(" \t\r\n");
      if (b == std::string::npos) continue;

      manifest_map[{x, y}] = file.substr(b, c - b + 1);
    }
  }

  void load_all_textures() {
    fs::path dir = fs::path(expand_home((std::string)directory));

    for (int x = 0; x < grid_size.width; x++) {
      for (int y = 0; y < grid_size.height; y++) {
        textures[x][y].destroy();

        std::optional<fs::path> img_path;
        auto it = manifest_map.find({x, y});
        if (it != manifest_map.end()) {
          const std::string& f = it->second;
          if (!f.empty() && ((f[0] == '/') || (f[0] == '~')))
            img_path = fs::path(expand_home(f));
          else
            img_path = dir / f;
        } else {
          std::string base = format_base_name(x, y);
          img_path = find_image_for_cell(dir, base);
        }

        if (!img_path || !fs::exists(*img_path)) {
          LOGW("workspace-wallpaper: missing wallpaper for ws ", x, ",", y,
               " -> ", img_path ? img_path->string() : std::string("(none)"));
          continue;
        }

        int iw = 0, ih = 0, comp = 0;
        stbi_uc *data = stbi_load(img_path->c_str(), &iw, &ih, &comp, 4);
        if (!data) {
          LOGE("workspace-wallpaper: failed to load ", img_path->string(), ": ",
               stbi_failure_reason());
          continue;
        }

        bool ok = upload_rgba_to_gl(textures[x][y], data, iw, ih);
        stbi_image_free(data);
        if (!ok)
            LOGE("workspace-wallpaper: upload failed ", img_path->string());
      }
    }

    output->render->damage_whole();
  }

  // Recompute node geometry AND the global cover rectangle for every cell.
  // The cover rect is fit once against the union, in global coords, so all
  // outputs sharing a cell get identical cover_* values -> seamless tiling.
  void apply_viewport(wf::point_t vp) {
    og = output->get_relative_geometry();

    wf::geometry_t lg = output->get_layout_geometry();  // this output, global
    wf::geometry_t total = layout_union();              // whole layout, global

    for (int x = 0; x < grid_size.width; x++) {
      for (int y = 0; y < grid_size.height; y++) {
        if (!nodes[x][y]) continue;

        nodes[x][y]->set_size(og.width, og.height);
        nodes[x][y]->set_position((x - vp.x) * og.width,
                                  (y - vp.y) * og.height);

        // Image dimensions for this cell (0,0 if not loaded -> fit_image
        // falls back to filling the span, which is fine).
        int iw = textures[x][y].valid ? textures[x][y].width : 0;
        int ih = textures[x][y].valid ? textures[x][y].height : 0;

        // Cover the image over the WHOLE union, in global coords. Identical on
        // every output for this cell.
        wlr_box cover = fit_image(current_mode, total.x, total.y, total.width,
                                  total.height, iw, ih);

        nodes[x][y]->set_cover(cover.x, cover.y, cover.width, cover.height,
                               lg.x, lg.y);
      }
    }
  }

  void recompute_all() {
    apply_viewport(output->wset()->get_current_workspace());
    output->render->damage_whole();
  }

  wf::signal::connection_t<wf::workspace_changed_signal> viewport_changed =
      [=](wf::workspace_changed_signal *ev) {
        apply_viewport(ev->new_viewport);
        output->render->damage_whole();
      };

  // This output's own geometry/scale changed.
  wf::signal::connection_t<wf::output_configuration_changed_signal>
      output_config_changed =
          [=](wf::output_configuration_changed_signal *ev) {
            if (!ev->changed_fields) return;
            if (ev->changed_fields & wf::OUTPUT_SOURCE_CHANGE) return;
            recompute_all();
          };

  // ANY output in the layout was added/removed/moved/resized. Each per-output
  // plugin instance connects this, so when the layout settles (e.g. kanshi
  // rotating the array at startup, or a hotplug), every instance recomputes
  // its cover rect against the now-complete union. This is what makes the
  // span robust to event ordering: no instance is left holding a partial-union
  // cover rect from mid-bringup.
  wf::signal::connection_t<wf::output_layout_configuration_changed_signal>
      layout_changed =
          [=](wf::output_layout_configuration_changed_signal *) {
            recompute_all();
          };

  wf::config::option_base_t::updated_callback_t cfg_changed = [=]() {
    current_mode = parse_mode((std::string)mode_opt);
    load_manifest();

    for (int x = 0; x < grid_size.width; x++) {
      for (int y = 0; y < grid_size.height; y++) {
        if (nodes[x][y]) nodes[x][y]->mode = current_mode;
      }
    }

    load_all_textures();
    recompute_all();  // image sizes changed -> cover rects must follow
  };

 public:
  void init() override {
    if (!wf::get_core().is_gles2()) {
      LOGE("workspace-wallpaper plugin requires GLES2 renderer!");
      return;
    }

    black_tex = create_solid_rgba_texture(0, 0, 0, 255);

    output->connect(&viewport_changed);
    output->connect(&output_config_changed);
    wf::get_core().output_layout->connect(&layout_changed);

    directory.set_callback(cfg_changed);
    pattern.set_callback(cfg_changed);
    mode_opt.set_callback(cfg_changed);
    manifest.set_callback(cfg_changed);

    grid_size = output->wset()->get_workspace_grid_size();
    textures.resize(grid_size.width);
    for (int x = 0; x < grid_size.width; x++)
      textures[x].resize(grid_size.height);

    current_mode = parse_mode((std::string)mode_opt);

    rebuild_nodes();
    load_manifest();
    load_all_textures();
    recompute_all();  // cover rects against textures we just loaded
  }

  void fini() override {
    for (int x = 0; x < grid_size.width; x++)
      for (int y = 0; y < grid_size.height; y++) textures[x][y].destroy();

    if (black_tex != (GLuint)-1) {
      GLuint t = black_tex;
      wf::gles::run_in_context([&] { glDeleteTextures(1, &t); });
      black_tex = (GLuint)-1;
    }

    // Nodes are shared_ptrs attached to the scene; clearing our refs is enough.
    nodes.clear();
    output->render->damage_whole();
  }
};

DECLARE_WAYFIRE_PLUGIN(wf::per_output_plugin_t<wayfire_workspace_wallpaper>);
