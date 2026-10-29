#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef G_OS_WIN32
#include <glib/gwin32.h>
#endif

#define APP_ID "org.timeregular.TimeRegular"
#define APP_NAME "TimeRegular"
#define ALARM_SOUND "/usr/share/sounds/freedesktop/stereo/alarm-clock-elapsed.oga"

typedef enum {
  MODE_WORK = 0,
  MODE_BREAK = 1
} Mode;

typedef enum {
  ST_IDLE = 0,
  ST_RUNNING,
  ST_PAUSED,
  ST_ALARM
} State;

static const int WORK_PRESETS[] = { 5, 10, 15, 20, 25, 30, 45, 60 };
static const int BREAK_PRESETS[] = { 5, 10, 15, 20, 30 };

typedef struct {
  GtkApplication *application;
  GtkWidget *window;
  GtkWidget *overlay;

  GtkWidget *segmented;
  GtkWidget *mode_work_btn;
  GtkWidget *mode_break_btn;

  GtkWidget *clock_label;
  GtkWidget *progress;
  GtkWidget *status_label;

  GtkWidget *chip_flow;
  GtkWidget *spin;

  GtkWidget *start_button;
  GtkWidget *reset_button;

  GtkWidget *alarm_revealer;
  GtkWidget *alarm_title;
  GtkWidget *alarm_desc;
  GtkWidget *alarm_next;
  GtkWidget *alarm_repeat;

  Mode mode;
  State state;
  int work_min;
  int break_min;

  gdouble remaining;
  gint64 end_us;
  guint tick_id;
  guint alarm_id;

  gboolean updating;
} App;

static App app;

static gboolean debug_enabled(void)
{
  static int enabled = -1;
  if (enabled < 0)
    enabled = (g_getenv("TIMEREGULAR_DEBUG") != NULL) ? 1 : 0;
  return enabled == 1;
}

#define DBG(...) do { if (debug_enabled()) g_print(__VA_ARGS__); } while (0)

static gboolean tick_cb(gpointer data);
static void trigger_alarm(void);
static void start_timer(void);
static void prepare_mode(Mode mode, State state, gboolean start_now);
static void on_chip_clicked(GtkButton *button, gpointer data);
static void on_alarm_next_clicked(GtkButton *button, gpointer data);

/* ---------------------------------------------------------------- settings */

static gdouble total_seconds(void)
{
  const gchar *override = g_getenv("TIMEREGULAR_TEST_SECONDS");
  int minutes;

  if (override != NULL) {
    int seconds = atoi(override);
    if (seconds > 0)
      return (gdouble)seconds;
  }

  minutes = (app.mode == MODE_WORK) ? app.work_min : app.break_min;
  return (gdouble)minutes * 60.0;
}

static int current_minutes(void)
{
  return (app.mode == MODE_WORK) ? app.work_min : app.break_min;
}

static int clamp_minutes(int minutes)
{
  if (minutes < 1)
    return 1;
  if (minutes > 180)
    return 180;
  return minutes;
}

static void pkill_pattern(const gchar *pattern)
{
#ifndef G_OS_WIN32
  gchar *command = g_strdup_printf("pkill -f %s", pattern);
  g_spawn_command_line_async(command, NULL);
  g_free(command);
#else
  (void)pattern;
#endif
}

static void play_alarm_sound(void)
{
#ifdef G_OS_WIN32
  g_spawn_command_line_async(
      "powershell -NoProfile -WindowStyle Hidden -Command "
      "[console]::beep(880,600);[console]::beep(660,600)",
      NULL);
#else
  gchar *command;

  if (g_file_test(ALARM_SOUND, G_FILE_TEST_EXISTS)) {
    command = g_strdup_printf("paplay %s", ALARM_SOUND);
    g_spawn_command_line_async(command, NULL);
    g_free(command);
    return;
  }

  g_spawn_command_line_async("canberra-gtk-play -i alarm-clock-elapsed", NULL);
#endif
}

static void stop_alarm_sound(void)
{
  if (app.alarm_id != 0) {
    g_source_remove(app.alarm_id);
    app.alarm_id = 0;
  }

  pkill_pattern("alarm-clock-elapsed");
  pkill_pattern("freedesktop/stereo/complete");
}

static gboolean alarm_repeat_cb(gpointer data)
{
  (void)data;
  play_alarm_sound();
  return G_SOURCE_CONTINUE;
}

static void send_notification(void)
{
  if (g_find_program_in_path("notify-send") == NULL)
    return;

  if (app.mode == MODE_WORK) {
    g_spawn_command_line_async(
        "notify-send -a TimeRegular -u critical 'Время вышло' 'Фокус завершён — пора на перерыв.'",
        NULL);
  } else {
    g_spawn_command_line_async(
        "notify-send -a TimeRegular -u critical 'Время вышло' 'Перерыв завершён — возвращайся к работе.'",
        NULL);
  }
}

static gchar *config_path(void)
{
  return g_build_filename(g_get_user_config_dir(), "timeregular", "config.ini", NULL);
}

static void ensure_config_dir(void)
{
  gchar *dir = g_build_filename(g_get_user_config_dir(), "timeregular", NULL);
  g_mkdir_with_parents(dir, 0700);
  g_free(dir);
}

static void load_config(void)
{
  GKeyFile *key_file = g_key_file_new();
  gchar *path = config_path();

  app.work_min = 25;
  app.break_min = 5;

  if (g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, NULL)) {
    app.work_min = g_key_file_get_integer(key_file, "timing", "work_minutes", NULL);
    app.break_min = g_key_file_get_integer(key_file, "timing", "break_minutes", NULL);
  }

  app.work_min = clamp_minutes(app.work_min);
  app.break_min = clamp_minutes(app.break_min);

  g_key_file_free(key_file);
  g_free(path);
}

static void save_config(void)
{
  GKeyFile *key_file = g_key_file_new();
  gchar *data;
  gsize length = 0;
  gchar *path = config_path();

  ensure_config_dir();
  g_key_file_set_integer(key_file, "timing", "work_minutes", app.work_min);
  g_key_file_set_integer(key_file, "timing", "break_minutes", app.break_min);

  data = g_key_file_to_data(key_file, &length, NULL);
  g_file_set_contents(path, data, (gssize)length, NULL);

  g_free(data);
  g_key_file_free(key_file);
  g_free(path);
}

/* -------------------------------------------------------------------- view */

static void set_css_class(GtkWidget *widget, const gchar *name, gboolean enabled)
{
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  if (enabled)
    gtk_style_context_add_class(context, name);
  else
    gtk_style_context_remove_class(context, name);
}

static const int *current_presets(int *count)
{
  if (app.mode == MODE_WORK) {
    *count = (int)(sizeof(WORK_PRESETS) / sizeof(WORK_PRESETS[0]));
    return WORK_PRESETS;
  }
  *count = (int)(sizeof(BREAK_PRESETS) / sizeof(BREAK_PRESETS[0]));
  return BREAK_PRESETS;
}

static void sync_chip_selection(void)
{
  GList *children;
  GList *item;
  int active = current_minutes();

  if (app.chip_flow == NULL)
    return;

  children = gtk_container_get_children(GTK_CONTAINER(app.chip_flow));
  for (item = children; item != NULL; item = item->next) {
    GtkWidget *chip = GTK_WIDGET(item->data);
    int minutes = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(chip), "minutes"));
    set_css_class(chip, "selected", minutes == active);
  }
  g_list_free(children);
}

static void update_display(void)
{
  gchar *text;
  int seconds;
  gdouble total;
  gdouble fraction;
  const gchar *status;
  gboolean editable;
  gboolean is_break;

  seconds = (int)ceil(app.remaining - 1e-6);
  if (seconds < 0)
    seconds = 0;

  text = g_strdup_printf("%02d:%02d", seconds / 60, seconds % 60);
  gtk_label_set_text(GTK_LABEL(app.clock_label), text);
  g_free(text);

  total = total_seconds();
  fraction = (total > 0.0) ? app.remaining / total : 0.0;
  if (fraction < 0.0)
    fraction = 0.0;
  if (fraction > 1.0)
    fraction = 1.0;
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(app.progress), fraction);

  switch (app.state) {
    case ST_IDLE:
      status = (app.mode == MODE_WORK) ? "Готов к работе" : "Готов к перерыву";
      break;
    case ST_RUNNING:
      status = (app.mode == MODE_WORK) ? "Идёт фокус" : "Идёт перерыв";
      break;
    case ST_PAUSED:
      status = "Пауза";
      break;
    default:
      status = "Время вышло";
      break;
  }
  gtk_label_set_text(GTK_LABEL(app.status_label), status);

  gtk_button_set_label(GTK_BUTTON(app.start_button),
                       app.state == ST_RUNNING ? "Пауза" :
                       app.state == ST_PAUSED ? "Продолжить" : "Старт");

  is_break = (app.mode == MODE_BREAK);
  set_css_class(app.start_button, "break", is_break);
  set_css_class(app.progress, "break", is_break);

  editable = (app.state == ST_IDLE || app.state == ST_PAUSED);
  gtk_widget_set_sensitive(app.chip_flow, editable);
  gtk_widget_set_sensitive(app.spin, editable);
  gtk_widget_set_sensitive(app.segmented, editable);
}

static void rebuild_chips(void)
{
  GList *children;
  GList *item;
  int count = 0;
  int i;
  const int *presets;

  children = gtk_container_get_children(GTK_CONTAINER(app.chip_flow));
  for (item = children; item != NULL; item = item->next)
    gtk_widget_destroy(GTK_WIDGET(item->data));
  g_list_free(children);

  presets = current_presets(&count);
  for (i = 0; i < count; i++) {
    gchar *label = g_strdup_printf("%d", presets[i]);
    GtkWidget *chip = gtk_button_new_with_label(label);

    gtk_style_context_add_class(gtk_widget_get_style_context(chip), "chip");
    g_object_set_data(G_OBJECT(chip), "minutes", GINT_TO_POINTER(presets[i]));
    g_signal_connect(chip, "clicked", G_CALLBACK(on_chip_clicked), NULL);
    gtk_flow_box_insert(GTK_FLOW_BOX(app.chip_flow), chip, -1);
    g_free(label);
  }

  gtk_widget_show_all(app.chip_flow);
  sync_chip_selection();
}

static void set_minutes(int minutes, gboolean reset_timer)
{
  minutes = clamp_minutes(minutes);

  if (app.mode == MODE_WORK)
    app.work_min = minutes;
  else
    app.break_min = minutes;

  save_config();

  if (reset_timer && app.state != ST_RUNNING && app.state != ST_ALARM) {
    if (app.tick_id != 0) {
      g_source_remove(app.tick_id);
      app.tick_id = 0;
    }
    app.remaining = total_seconds();
    app.state = ST_IDLE;
  }

  update_display();
}

/* ------------------------------------------------------------------ timers */

static void start_timer(void)
{
  DBG("start_timer rem=%.2f tick=%u\n", app.remaining, app.tick_id);
  if (app.remaining <= 0.0)
    app.remaining = total_seconds();

  app.state = ST_RUNNING;
  app.end_us = g_get_monotonic_time() + (gint64)(app.remaining * 1000000.0);

  if (app.tick_id == 0)
    app.tick_id = g_timeout_add(100, tick_cb, NULL);

  DBG("start_timer now state=%d tick=%u\n", app.state, app.tick_id);
  update_display();
}

static gboolean tick_cb(gpointer data)
{
  gint64 now;

  (void)data;

  if (app.state != ST_RUNNING) {
    app.tick_id = 0;
    return G_SOURCE_REMOVE;
  }

  now = g_get_monotonic_time();
  app.remaining = (gdouble)(app.end_us - now) / 1000000.0;

  if (app.remaining <= 0.0) {
    app.remaining = 0.0;
    app.tick_id = 0;
    trigger_alarm();
    return G_SOURCE_REMOVE;
  }

  update_display();
  return G_SOURCE_CONTINUE;
}

static void pause_timer(void)
{
  gint64 now;

  now = g_get_monotonic_time();
  app.remaining = (gdouble)(app.end_us - now) / 1000000.0;
  if (app.remaining < 0.0)
    app.remaining = 0.0;

  app.state = ST_PAUSED;
  update_display();
}

static void reset_timer(void)
{
  if (app.state == ST_ALARM)
    return;

  app.state = ST_IDLE;
  app.remaining = total_seconds();
  update_display();
}

static void set_alarm_visible(gboolean visible)
{
  if (visible) {
    gtk_widget_set_no_show_all(app.alarm_revealer, FALSE);
    gtk_widget_show_all(app.alarm_revealer);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(app.overlay),
                                         app.alarm_revealer, FALSE);
    gtk_revealer_set_reveal_child(GTK_REVEALER(app.alarm_revealer), TRUE);
  } else {
    gtk_revealer_set_reveal_child(GTK_REVEALER(app.alarm_revealer), FALSE);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(app.overlay),
                                         app.alarm_revealer, TRUE);
    gtk_widget_set_visible(app.alarm_revealer, FALSE);
    gtk_widget_set_no_show_all(app.alarm_revealer, TRUE);
  }
}

static void trigger_alarm(void)
{
  DBG("trigger_alarm mode=%d\n", app.mode);
  if (app.tick_id != 0) {
    g_source_remove(app.tick_id);
    app.tick_id = 0;
  }

  app.state = ST_ALARM;
  app.remaining = 0.0;

  if (app.mode == MODE_WORK) {
    gtk_label_set_text(GTK_LABEL(app.alarm_title), "Время вышло");
    gtk_label_set_text(GTK_LABEL(app.alarm_desc), "Фокус завершён. Далее — перерыв.");
    gtk_button_set_label(GTK_BUTTON(app.alarm_next), "Перейти к перерыву");
  } else {
    gtk_label_set_text(GTK_LABEL(app.alarm_title), "Время вышло");
    gtk_label_set_text(GTK_LABEL(app.alarm_desc), "Перерыв завершён. Далее — работа.");
    gtk_button_set_label(GTK_BUTTON(app.alarm_next), "Перейти к работе");
  }

  set_alarm_visible(TRUE);

  play_alarm_sound();
  if (app.alarm_id == 0)
    app.alarm_id = g_timeout_add(1500, alarm_repeat_cb, NULL);

  send_notification();
  update_display();
}

static void prepare_mode(Mode mode, State state, gboolean start_now)
{
  app.mode = mode;
  app.state = state;

  app.updating = TRUE;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(
      mode == MODE_WORK ? app.mode_work_btn : app.mode_break_btn), TRUE);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.spin), current_minutes());
  app.updating = FALSE;

  app.remaining = total_seconds();
  rebuild_chips();
  update_display();

  if (start_now)
    start_timer();
}

/* --------------------------------------------------------------- callbacks */

static gboolean on_window_button_press(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
  (void)widget;
  (void)data;
  DBG("window button-press x=%.0f y=%.0f button=%u\n", event->x, event->y, event->button);
  return FALSE;
}

static void on_start_clicked(GtkButton *button, gpointer data)
{
  (void)button;
  (void)data;
  DBG("on_start_clicked state=%d rem=%.2f\n", app.state, app.remaining);

  if (app.state == ST_RUNNING)
    pause_timer();
  else if (app.state == ST_ALARM)
    on_alarm_next_clicked(NULL, NULL);
  else
    start_timer();
}

static void on_reset_clicked(GtkButton *button, gpointer data)
{
  (void)button;
  (void)data;
  DBG("on_reset_clicked state=%d\n", app.state);
  reset_timer();
}

static void on_mode_toggled(GtkToggleButton *button, gpointer data)
{
  (void)data;
  DBG("on_mode_toggled updating=%d active=%d\n", app.updating, gtk_toggle_button_get_active(button));

  if (app.updating || !gtk_toggle_button_get_active(button))
    return;

  if (button == GTK_TOGGLE_BUTTON(app.mode_work_btn))
    prepare_mode(MODE_WORK, ST_IDLE, FALSE);
  else
    prepare_mode(MODE_BREAK, ST_IDLE, FALSE);
}

static void on_chip_clicked(GtkButton *button, gpointer data)
{
  int minutes;

  (void)data;

  if (!gtk_widget_get_sensitive(GTK_WIDGET(button)))
    return;

  minutes = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "minutes"));

  app.updating = TRUE;
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.spin), minutes);
  app.updating = FALSE;

  set_minutes(minutes, TRUE);
  sync_chip_selection();
}

static void on_spin_changed(GtkSpinButton *spin, gpointer data)
{
  int minutes;

  (void)data;

  if (app.updating)
    return;

  minutes = gtk_spin_button_get_value_as_int(spin);
  set_minutes(minutes, TRUE);
  sync_chip_selection();
}

static void on_alarm_next_clicked(GtkButton *button, gpointer data)
{
  Mode next;

  (void)button;
  (void)data;

  stop_alarm_sound();
  set_alarm_visible(FALSE);

  next = (app.mode == MODE_WORK) ? MODE_BREAK : MODE_WORK;
  prepare_mode(next, ST_IDLE, FALSE);
}

static void on_alarm_repeat_clicked(GtkButton *button, gpointer data)
{
  (void)button;
  (void)data;

  stop_alarm_sound();
  set_alarm_visible(FALSE);
  prepare_mode(app.mode, ST_IDLE, TRUE);
}

static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
  (void)widget;
  (void)data;
  DBG("on_key_press key=0x%x state=%d\n", event->keyval, app.state);

  switch (event->keyval) {
    case GDK_KEY_space:
    case GDK_KEY_Return:
      if (app.state == ST_ALARM)
        on_alarm_next_clicked(NULL, NULL);
      else
        on_start_clicked(NULL, NULL);
      return TRUE;
    case GDK_KEY_r:
    case GDK_KEY_R:
      on_reset_clicked(NULL, NULL);
      return TRUE;
    case GDK_KEY_Escape:
      if (app.state == ST_ALARM)
        on_alarm_next_clicked(NULL, NULL);
      return TRUE;
    default:
      return FALSE;
  }
}

/* ------------------------------------------------------------------- build */

static gchar *find_css_path(void)
{
  const gchar *candidates[] = {
    "/usr/share/timeregular/style.css",
    "style.css",
    "src/style.css",
    NULL
  };
#ifndef G_OS_WIN32
  gchar exe_path[4096];
  gssize len;
#endif
  int i;

#ifdef G_OS_WIN32
  {
    gchar *dir = g_win32_get_package_installation_directory_of_module(NULL);

    if (dir != NULL) {
      gchar *candidate = g_build_filename(dir, "style.css", NULL);

      if (g_file_test(candidate, G_FILE_TEST_EXISTS)) {
        g_free(dir);
        return candidate;
      }
      g_free(candidate);

      candidate = g_build_filename(dir, "share", "timeregular", "style.css", NULL);
      if (g_file_test(candidate, G_FILE_TEST_EXISTS)) {
        g_free(dir);
        return candidate;
      }
      g_free(candidate);
      g_free(dir);
    }
  }
#else
  len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
  if (len > 0) {
    gchar *dir;
    gchar *candidate;

    exe_path[len] = '\0';
    dir = g_path_get_dirname(exe_path);

    candidate = g_build_filename(dir, "style.css", NULL);
    if (g_file_test(candidate, G_FILE_TEST_EXISTS))
      return candidate;
    g_free(candidate);

    candidate = g_build_filename(dir, "..", "src", "style.css", NULL);
    if (g_file_test(candidate, G_FILE_TEST_EXISTS))
      return candidate;
    g_free(candidate);

    g_free(dir);
  }
#endif

  for (i = 0; candidates[i] != NULL; i++) {
    if (g_file_test(candidates[i], G_FILE_TEST_EXISTS))
      return g_strdup(candidates[i]);
  }

  return NULL;
}

static GtkWidget *make_label(const gchar *text, const gchar *css_class)
{
  GtkWidget *label = gtk_label_new(text);
  gtk_style_context_add_class(gtk_widget_get_style_context(label), css_class);
  return label;
}

static void on_activate(GtkApplication *application, gpointer data)
{
  GtkWidget *overlay;
  GtkWidget *root;
  GtkWidget *header;
  GtkWidget *titles;
  GtkWidget *segmented;
  GtkWidget *card;
  GtkWidget *presets_label;
  GtkWidget *custom_row;
  GtkWidget *custom_label;
  GtkWidget *minutes_label;
  GtkWidget *spacer;
  GtkWidget *controls;
  GtkWidget *scrim;
  GtkWidget *alarm;
  GtkWidget *alarm_buttons;
  GtkWidget *revealer;
  GtkCssProvider *css;
  gchar *css_path;

  (void)data;

  if (app.window != NULL) {
    gtk_window_present(GTK_WINDOW(app.window));
    return;
  }

  load_config();
  app.mode = MODE_WORK;
  app.state = ST_IDLE;
  app.remaining = total_seconds();

  gtk_window_set_default_icon_name("timeregular");

  app.window = gtk_application_window_new(application);
  gtk_window_set_title(GTK_WINDOW(app.window), APP_NAME);
  gtk_window_set_default_size(GTK_WINDOW(app.window), 420, 660);
  gtk_window_set_resizable(GTK_WINDOW(app.window), TRUE);
  gtk_widget_set_name(app.window, "appwindow");
  g_signal_connect(app.window, "key-press-event", G_CALLBACK(on_key_press), NULL);
  g_signal_connect(app.window, "button-press-event", G_CALLBACK(on_window_button_press), NULL);

  css = gtk_css_provider_new();
  css_path = find_css_path();
  if (css_path != NULL) {
    gtk_css_provider_load_from_path(css, css_path, NULL);
    g_free(css_path);
  }
  gtk_style_context_add_provider_for_screen(
      gdk_screen_get_default(),
      GTK_STYLE_PROVIDER(css),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(css);

  overlay = gtk_overlay_new();
  app.overlay = overlay;
  gtk_container_add(GTK_CONTAINER(app.window), overlay);

  root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
  gtk_widget_set_margin_start(root, 24);
  gtk_widget_set_margin_end(root, 24);
  gtk_widget_set_margin_top(root, 22);
  gtk_widget_set_margin_bottom(root, 22);
  gtk_container_add(GTK_CONTAINER(overlay), root);

  header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  titles = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_box_pack_start(GTK_BOX(titles), make_label("TimeRegular", "title"), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(titles), make_label("Фокус · перерыв", "subtitle"), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(header), titles, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(root), header, FALSE, FALSE, 0);

  segmented = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_box_set_homogeneous(GTK_BOX(segmented), TRUE);
  gtk_style_context_add_class(gtk_widget_get_style_context(segmented), "segmented");
  app.segmented = segmented;

  app.mode_work_btn = gtk_radio_button_new_with_label(NULL, "Работа");
  app.mode_break_btn = gtk_radio_button_new_with_label_from_widget(
      GTK_RADIO_BUTTON(app.mode_work_btn), "Перерыв");
  gtk_style_context_add_class(gtk_widget_get_style_context(app.mode_work_btn), "mode-btn");
  gtk_style_context_add_class(gtk_widget_get_style_context(app.mode_break_btn), "mode-btn");
  gtk_box_pack_start(GTK_BOX(segmented), app.mode_work_btn, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(segmented), app.mode_break_btn, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(root), segmented, FALSE, FALSE, 0);

  card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_style_context_add_class(gtk_widget_get_style_context(card), "card");
  gtk_widget_set_margin_top(card, 4);

  app.clock_label = make_label("00:00", "clock");
  gtk_widget_set_halign(app.clock_label, GTK_ALIGN_CENTER);
  gtk_box_pack_start(GTK_BOX(card), app.clock_label, FALSE, FALSE, 0);

  app.progress = gtk_progress_bar_new();
  gtk_style_context_add_class(gtk_widget_get_style_context(app.progress), "timebar");
  gtk_widget_set_valign(app.progress, GTK_ALIGN_CENTER);
  gtk_box_pack_start(GTK_BOX(card), app.progress, FALSE, FALSE, 6);

  app.status_label = make_label("Готов к работе", "status");
  gtk_widget_set_halign(app.status_label, GTK_ALIGN_CENTER);
  gtk_box_pack_start(GTK_BOX(card), app.status_label, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(root), card, FALSE, FALSE, 0);

  presets_label = make_label("Длительность, мин", "section");
  gtk_widget_set_margin_top(presets_label, 6);
  gtk_box_pack_start(GTK_BOX(root), presets_label, FALSE, FALSE, 0);

  app.chip_flow = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(app.chip_flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(app.chip_flow), TRUE);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(app.chip_flow), 8);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(app.chip_flow), 8);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(app.chip_flow), 4);
  gtk_widget_set_halign(app.chip_flow, GTK_ALIGN_FILL);
  gtk_box_pack_start(GTK_BOX(root), app.chip_flow, FALSE, FALSE, 0);

  custom_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_halign(custom_row, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(custom_row, 4);
  custom_label = make_label("Своё", "muted");
  app.spin = gtk_spin_button_new_with_range(1, 180, 1);
  gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(app.spin), TRUE);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(app.spin), current_minutes());
  gtk_widget_set_size_request(app.spin, 84, -1);
  minutes_label = make_label("мин", "muted");
  gtk_box_pack_start(GTK_BOX(custom_row), custom_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(custom_row), app.spin, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(custom_row), minutes_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(root), custom_row, FALSE, FALSE, 0);

  spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_vexpand(spacer, TRUE);
  gtk_box_pack_start(GTK_BOX(root), spacer, TRUE, TRUE, 0);

  controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  app.start_button = gtk_button_new_with_label("Старт");
  app.reset_button = gtk_button_new_with_label("Сброс");
  gtk_style_context_add_class(gtk_widget_get_style_context(app.start_button), "primary");
  gtk_style_context_add_class(gtk_widget_get_style_context(app.reset_button), "ghost");
  gtk_widget_set_hexpand(app.start_button, TRUE);
  gtk_box_pack_start(GTK_BOX(controls), app.start_button, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(controls), app.reset_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(root), controls, FALSE, FALSE, 0);

  revealer = gtk_revealer_new();
  gtk_revealer_set_transition_type(GTK_REVEALER(revealer),
                                   GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
  gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), 160);
  gtk_widget_set_halign(revealer, GTK_ALIGN_FILL);
  gtk_widget_set_valign(revealer, GTK_ALIGN_FILL);
  gtk_widget_set_hexpand(revealer, TRUE);
  gtk_widget_set_vexpand(revealer, TRUE);
  app.alarm_revealer = revealer;

  scrim = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_style_context_add_class(gtk_widget_get_style_context(scrim), "scrim");

  alarm = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_style_context_add_class(gtk_widget_get_style_context(alarm), "alarm");
  gtk_widget_set_halign(alarm, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(alarm, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(alarm, 300, -1);

  app.alarm_title = make_label("Время вышло", "alarm-title");
  gtk_widget_set_halign(app.alarm_title, GTK_ALIGN_CENTER);
  app.alarm_desc = make_label("", "alarm-desc");
  gtk_widget_set_halign(app.alarm_desc, GTK_ALIGN_CENTER);
  gtk_label_set_line_wrap(GTK_LABEL(app.alarm_desc), TRUE);
  gtk_label_set_justify(GTK_LABEL(app.alarm_desc), GTK_JUSTIFY_CENTER);

  gtk_box_pack_start(GTK_BOX(alarm), app.alarm_title, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(alarm), app.alarm_desc, FALSE, FALSE, 0);

  alarm_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(alarm_buttons, 8);
  app.alarm_next = gtk_button_new_with_label("Далее");
  app.alarm_repeat = gtk_button_new_with_label("Повторить");
  gtk_style_context_add_class(gtk_widget_get_style_context(app.alarm_next), "primary");
  gtk_style_context_add_class(gtk_widget_get_style_context(app.alarm_repeat), "ghost");
  gtk_widget_set_hexpand(app.alarm_next, TRUE);
  gtk_box_pack_start(GTK_BOX(alarm_buttons), app.alarm_next, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(alarm_buttons), app.alarm_repeat, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(alarm), alarm_buttons, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(scrim), alarm, TRUE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(revealer), scrim);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), revealer);

  /* Keep the alarm layer out of the input chain while it is not active. */
  gtk_widget_set_no_show_all(revealer, TRUE);
  gtk_widget_set_visible(revealer, FALSE);
  gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay), revealer, TRUE);

  g_signal_connect(app.mode_work_btn, "toggled", G_CALLBACK(on_mode_toggled), NULL);
  g_signal_connect(app.mode_break_btn, "toggled", G_CALLBACK(on_mode_toggled), NULL);
  g_signal_connect(app.start_button, "clicked", G_CALLBACK(on_start_clicked), NULL);
  g_signal_connect(app.reset_button, "clicked", G_CALLBACK(on_reset_clicked), NULL);
  g_signal_connect(app.spin, "value-changed", G_CALLBACK(on_spin_changed), NULL);
  g_signal_connect(app.alarm_next, "clicked", G_CALLBACK(on_alarm_next_clicked), NULL);
  g_signal_connect(app.alarm_repeat, "clicked", G_CALLBACK(on_alarm_repeat_clicked), NULL);

  app.updating = TRUE;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app.mode_work_btn), TRUE);
  app.updating = FALSE;

  rebuild_chips();
  update_display();

  gtk_widget_show_all(app.window);

  DBG("activate done css=%s\n", css_path ? "loaded" : "missing");
}

int main(int argc, char **argv)
{
  GtkApplication *application;
  int status;

  memset(&app, 0, sizeof(app));

  g_set_prgname("timeregular");
  g_set_application_name(APP_NAME);

  gtk_init(&argc, &argv);
  g_object_set(gtk_settings_get_default(),
               "gtk-application-prefer-dark-theme", TRUE, NULL);

  application = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(application, "activate", G_CALLBACK(on_activate), NULL);

  status = g_application_run(G_APPLICATION(application), argc, argv);

  g_object_unref(application);
  return status;
}
