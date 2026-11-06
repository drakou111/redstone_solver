package io.github.drakou111.redstonesolver.client;

import com.google.gson.GsonBuilder;
import com.google.gson.JsonObject;
import com.google.gson.JsonParseException;
import com.google.gson.JsonParser;
import io.github.drakou111.redstonesolver.core.CircuitDraft;
import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Optional;

/**
 * Drafts saved as JSON files in config/redstone_solver/drafts: named saves, plus one automatic
 * save per world or server so the draft survives leaving the game.
 */
public final class DraftFiles {
    public static final String AUTOSAVE_PREFIX = "autosave-";
    private static final String EXTENSION = ".json";
    private static final String NAME_PATTERN = "[A-Za-z0-9_-]+";
    private static final int MAX_NAME_LENGTH = 64;

    private final Path directory;

    public DraftFiles(Path configDir) {
        this.directory = configDir.resolve("redstone_solver").resolve("drafts");
    }

    public Path getDirectory() {
        return this.directory;
    }

    /** Saves the draft and solve settings; refuses to replace an existing named save unless asked. */
    public void save(String name, ClientState state, boolean overwrite) {
        Path file = this.fileFor(name);
        if (!overwrite && Files.exists(file))
            throw new IllegalStateException("A draft named " + name + " exists; add 'overwrite' to replace it.");
        JsonObject json = new JsonObject();
        json.add("draft", state.getDraft().toJson());
        JsonObject settings = new JsonObject();
        settings.addProperty("timeLimit", state.getTimeLimitSeconds());
        settings.addProperty("method", state.getMethod().name());
        settings.addProperty("onePlacement", state.isOnePlacement());
        settings.addProperty("sketchSeconds", state.getSketchSeconds());
        json.add("settings", settings);
        try {
            Files.createDirectories(this.directory);
            Path temporary = file.resolveSibling(file.getFileName() + ".tmp");
            Files.writeString(temporary, new GsonBuilder().setPrettyPrinting().create().toJson(json), StandardCharsets.UTF_8);
            Files.move(temporary, file, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
        } catch (IOException e) {
            throw new UncheckedIOException("Cannot save " + file + ": " + e.getMessage(), e);
        }
    }

    /** Loads a save into the state; false if there is no save of that name. */
    public boolean load(String name, ClientState state) {
        Path file = this.fileFor(name);
        if (!Files.exists(file)) return false;
        JsonObject json;
        try {
            json = JsonParser.parseString(Files.readString(file, StandardCharsets.UTF_8)).getAsJsonObject();
        } catch (IOException e) {
            throw new UncheckedIOException("Cannot read " + file + ": " + e.getMessage(), e);
        } catch (JsonParseException | IllegalStateException e) {
            throw new IllegalArgumentException(file.getFileName() + " is not valid JSON: " + e.getMessage(), e);
        }
        if (!json.has("draft")) throw new IllegalArgumentException(file.getFileName() + " has no draft in it");
        CircuitDraft draft = CircuitDraft.fromJson(json.getAsJsonObject("draft"));
        state.replaceDraft(draft);
        Optional.ofNullable(json.getAsJsonObject("settings")).ifPresent(settings -> {
            if (settings.has("timeLimit")) state.setTimeLimitSeconds(settings.get("timeLimit").getAsDouble());
            if (settings.has("method")) state.setMethod(ClientState.Method.valueOf(settings.get("method").getAsString()));
            if (settings.has("onePlacement")) state.setOnePlacement(settings.get("onePlacement").getAsBoolean());
            if (settings.has("sketchSeconds")) state.setSketchSeconds(settings.get("sketchSeconds").getAsDouble());
        });
        return true;
    }

    /** Names of the saved drafts (without automatic saves), sorted. */
    public List<String> listNames() {
        List<String> names = new ArrayList<>();
        if (!Files.isDirectory(this.directory)) return names;
        try (DirectoryStream<Path> files = Files.newDirectoryStream(this.directory, "*" + EXTENSION)) {
            for (Path file : files) {
                String name = file.getFileName().toString();
                name = name.substring(0, name.length() - EXTENSION.length());
                if (!name.startsWith(AUTOSAVE_PREFIX)) names.add(name);
            }
        } catch (IOException e) {
            throw new UncheckedIOException("Cannot list " + this.directory + ": " + e.getMessage(), e);
        }
        names.sort(String::compareTo);
        return names;
    }

    /** A file-safe automatic save name for a world or server. */
    public static String autosaveName(String worldId) {
        String safe = worldId.toLowerCase(Locale.ROOT).replaceAll("[^a-z0-9_-]+", "_");
        if (safe.length() > MAX_NAME_LENGTH) safe = safe.substring(0, MAX_NAME_LENGTH);
        return AUTOSAVE_PREFIX + safe;
    }

    private Path fileFor(String name) {
        if (!name.matches(NAME_PATTERN) || name.length() > MAX_NAME_LENGTH + AUTOSAVE_PREFIX.length())
            throw new IllegalArgumentException("Draft names use letters, digits, _ and - only.");
        return this.directory.resolve(name + EXTENSION);
    }
}
