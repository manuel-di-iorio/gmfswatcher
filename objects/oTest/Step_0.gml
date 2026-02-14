fswatcher_on_change(function(event, path, newPath) {
  if (event == "RENAMED") {
    show_debug_message(event + " - OLD: " + path + " - NEW: " + newPath);
  } else {
    show_debug_message(event + " - " + path);
  }
});