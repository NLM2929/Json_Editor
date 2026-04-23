#include"raylib.h"
#include"raymath.h"
#define RAYGUI_IMPLEMENTATION
#include"raygui.h"
#include<string>
#include<nlohmann/json.hpp>
#include<ctime>
#include<vector>
#include<thread>
#include<filesystem>
#include<iostream>
#include<fstream>
#include<mutex>
#include<algorithm>
#include<cctype>
#include<pdcurses.h>

#define TOTAL_AUTO_SAVE_PER_FILE 10
#define TOTAL_MESSAGE 500

struct TextureList{
    std::vector<std::string> textures;
    unsigned short target_layer = 0;
};

struct MapData{
    int width = 0, height = 0, tileSize = 0;
    std::string min_version = "", max_version = "";
    std::vector<std::vector<unsigned short>> layers;
    std::vector<TextureList> textureLists;
};

bool running = true;

std::mutex msgMutex;
std::vector<std::string> message;

std::mutex cmdMutex;
std::string command;
bool send = false;

void Print(std::string msg){
    {
        std::lock_guard<std::mutex> lock(msgMutex);
        if(message.size()>TOTAL_MESSAGE){
            message.erase(message.begin(), message.begin() + 100);
        }
        message.push_back(msg);
    }
}

int InitMap(MapData& mapData, std::filesystem::path name, bool checked = false){
    name.replace_extension(".json");
    if(std::filesystem::exists(name)&&!checked) {return 1;}
    mapData.layers.push_back(std::vector<unsigned short>(mapData.width*mapData.height, 0));
    return 0;
}

int SaveMap(MapData& map, std::filesystem::path filename){
    filename.replace_extension(".json");
    std::ofstream file(filename);
    if(!file.is_open()) {return 1;}
    std::string indent = "\t";
    file << "{\n"
    << indent << "\"width\": " << map.width << ",\n"
    << indent << "\"height\": " << map.height << ",\n"
    << indent << "\"tileSize\": " << map.tileSize << ",\n"
    << indent << "\"min_version\": \"" << map.min_version << "\",\n"
    << indent << "\"max_version\": \"" << map.max_version << "\",\n"
    << indent << "\"layers\": [\n";
    for(size_t i = 0; i < map.layers.size(); i++){
        file << indent << indent << "[\n" << indent << indent << indent;
        for(size_t j = 0; j < map.layers[i].size(); j++){
            file << map.layers[i][j];
            if(j!=map.layers[i].size()-1){
                file << ", ";
            }
            if((j+1)%map.width==0){
                file << "\n";
                if(j!=map.layers[i].size()-1){
                    file << indent << indent << indent;
                }
            }
        }
        file << "\n" << indent << indent << "]";
        if(i!=map.layers.size()-1){
            file << ",";
        }
        file << "\n";
    }
    file << indent << "],\n";
    file << indent << "\"textureLists\": [\n";
    for(size_t i = 0; i < map.textureLists.size(); i++){
        file << indent << indent << "{\n" << indent << indent << indent
        << "\"target_layer\": " << map.textureLists[i].target_layer << ",\n"
        << indent << indent << indent
        << "\"textures\": [\n";
        for(size_t j = 0; j < map.textureLists[i].textures.size(); j++){
            file << indent << indent << indent << indent
            << "\"" << map.textureLists[i].textures[j] << "\"";
            if(j!=map.textureLists[i].textures.size()-1){
                file << ",";
            }
            file << "\n";
        }
        file << indent << indent << indent << "]\n";
        file << indent << indent << "}";
        if(i!=map.textureLists.size()-1){
            file << ",";
        }
        file << "\n";
    }
    file << indent << "]\n";
    file << "}";
    file.close();
    Print(TextFormat("Saved map to %s\n", filename.string().c_str()));
    return 0;
}

void AutoSave(const double latest, const double& lastAutoSave, const double& autoSaveTime,
const std::string& file_name, const std::string& path, MapData& map){
    auto time_stamp = [](){
        char b[18];  // this finally works
        std::time_t t = std::time(nullptr);
        std::strftime(b, sizeof(b), "_%Y%m%d_%H%M%S", std::localtime(&t));
        return std::string(b);
        //the other one
        // std::string s = std::format(
        //     "_{:%Y%m%d_%H%M%S}",
        //     std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())
        // );
        // return s;
    };
    if(latest-lastAutoSave>=autoSaveTime){
        std::vector<std::filesystem::directory_entry> auto_saves;
        for(const auto& entry : std::filesystem::directory_iterator(path)) {
            if(entry.is_regular_file()) {
                const auto& file = entry.path();
                if(file.string().find("AUTO_SAVE_" + file_name) != std::string::npos) {
                    auto_saves.push_back(entry);
                }
            }
        }
        if(auto_saves.size() > TOTAL_AUTO_SAVE_PER_FILE) {
            std::sort(auto_saves.begin(), auto_saves.end(), [](const std::filesystem::directory_entry& a, const std::filesystem::directory_entry& b) {
                return std::filesystem::last_write_time(a) < std::filesystem::last_write_time(b);
            });
            std::filesystem::remove(auto_saves.front());
        }
        SaveMap(map, path + "AUTO_SAVE_" + file_name
            + time_stamp()
        );
    }
}

int LoadMap(MapData& mapData, std::string name){
    std::ifstream file(name+".json");
    if(!file.is_open()) {return 1;}
    nlohmann::json j;
    file >> j;
    mapData.width = j["width"];
    mapData.height = j["height"];
    mapData.tileSize = j["tileSize"];
    mapData.min_version = j["min_version"];
    mapData.max_version = j["max_version"];
    for(const auto& layer_json : j["layers"]){
        std::vector<unsigned short> layer;
        for(const auto& tile : layer_json){
            layer.push_back(tile);
        }
        mapData.layers.push_back(layer);
    }

    file.close();
    return 0;
}

void DrawGrid2D(const Camera2D &cam, const int& mapW, const int& mapH, const int& tileSize){
    // Minimum spacing (in screen pixels) between drawn grid lines
    const float minPixelSpacing = 6.0f;    // tweak: 4..12
    const int majorEvery = 8;              // heavier line every N tiles

    // screen-space size of one tile
    float screenTile = tileSize * cam.zoom;
    // if a single tile is smaller than a few pixels, skip drawing the grid entirely
    // if (screenTile < 2.0f) return;

    // compute step in tiles so adjacent drawn lines are at least minPixelSpacing
    int step = std::max(1, (int)std::ceil(minPixelSpacing / screenTile));

    // find world-space rectangle visible on screen
    Vector2 tl = GetScreenToWorld2D({0,0}, cam);
    Vector2 br = GetScreenToWorld2D({(float)GetScreenWidth(), (float)GetScreenHeight()}, cam);

    // convert to tile indices with floor to be safe with negatives
    int startX = (int)floor(std::min(tl.x, br.x) / tileSize) - 1;
    int endX   = (int)floor(std::max(tl.x, br.x) / tileSize) + 1;
    int startY = (int)floor(std::min(tl.y, br.y) / tileSize) - 1;
    int endY   = (int)floor(std::max(tl.y, br.y) / tileSize) + 1;

    // clamp to map bounds
    startX = std::max(0, startX);
    startY = std::max(0, startY);
    endX   = std::min(mapW, endX);
    endY   = std::min(mapH, endY);

    Color minor = Fade(WHITE, 0.12f);
    Color major = Fade(WHITE, 0.25f);

    // draw horizontal lines (y)
    for (int y = startY; y <= endY; y += step)
    {
        bool isMajor = ( (y % majorEvery) == 0 );
        float wy = (float)y * tileSize;
        DrawLine((float)startX * tileSize, wy, (float)endX * tileSize, wy, isMajor ? major : minor);
    }

    // draw vertical lines (x)
    for (int x = startX; x <= endX; x += step)
    {
        bool isMajor = ( (x % majorEvery) == 0 );
        float wx = (float)x * tileSize;
        DrawLine(wx, (float)startY * tileSize, wx, (float)endY * tileSize, isMajor ? major : minor);
    }
}

void EmptyLog(int level, const char* text, va_list args){
    (void)level; (void)text; (void) args;
    return;
}

int main(){
    SetTraceLogLevel(LOG_NONE);
    SetTraceLogCallback(EmptyLog);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(800, 600, "Json map editor");
    SetWindowState(FLAG_WINDOW_MAXIMIZED);
    ClearWindowState(FLAG_WINDOW_RESIZABLE);
    GuiLoadStyle("./style_dark.rgs");
    MapData map = {0};
    bool IsReady = false;
    std::size_t current_layer = 0;
    std::thread CLI_Thread([&IsReady](){
        initscr();
        cbreak();
        noecho();
        keypad(stdscr, TRUE);
        nodelay(stdscr, TRUE);
        curs_set(0);
        int h=0, w=0, top_line = 0;
        getmaxyx(stdscr, h, w);
        WINDOW* logPad = newpad(TOTAL_MESSAGE, w);
        WINDOW* cmdWin = newwin(3, w, h-3, 0);
        Print("CLI Json Level Editor");
        Print(TextFormat("Current working directory: %s", std::filesystem::current_path().string().c_str()));
        scrollok(logPad, TRUE);
        box(cmdWin, 0, 0);
        mvwprintw(cmdWin, 1, 1, "> ");
        wrefresh(cmdWin);
        prefresh(logPad, top_line, 0, 0, 0, h-4, w-1);
        std::string input;
        while(running && !WindowShouldClose()){
            int ch = 0;
            if(IsReady){
                ch = getch();
            }
            if(GetMouseWheelMove()>0){
                if(top_line>0){
                    top_line--;
                }
            } else
            if(GetMouseWheelMove()<0){
                if(top_line < TOTAL_MESSAGE - (h - 4)){
                    top_line++;
                }
            }
            if(ch != ERR){
                if(ch=='\n'||ch=='\r'){
                    {
                        std::lock_guard<std::mutex> lock(cmdMutex);
                        command = input;
                        send = true;
                    }
                    input.clear();
                } else
                if(ch==KEY_BACKSPACE||ch==127||ch==8){
                    if(!input.empty()){
                        input.pop_back();
                    }
                } else
                if(ch>=32&&ch<=126){
                    input.push_back(ch);
                }
            }
            werase(logPad);
            {
                std::lock_guard<std::mutex> lock(msgMutex);
                for(std::size_t i = 0; i < message.size(); i++){
                    mvwprintw(logPad, i + 1, 1, message[i].c_str());
                }
            }
            prefresh(logPad, top_line, 0, 0, 0, h-4, w-1);
            werase(cmdWin);
            box(cmdWin, 0, 0);
            if(IsReady){
                mvwprintw(cmdWin, 1, 1, "> %s", input.c_str());
            }
            wrefresh(cmdWin);
            napms(50);
        }
        delwin(logPad);
        delwin(cmdWin);
        endwin();
    });
    Camera2D cam = {0};
    cam.zoom = 1.f;
    cam.target = {0.f,0.f};
    cam.offset = {GetScreenWidth()/2.f, GetScreenHeight()/2.f};
    double startedEditing = 0, autoSaveTime = 60*30, lastAutoSave = 0;
    char state = 1, error_state = 0;
    std::string file_name("Untitled");
    std::string path("./JsonFiles/");
    bool editing[4] = {0}, saved = false;
    auto Parse = [&map, &current_layer, &saved](){
        if(!send) {return;}
        std::vector<std::string> tokens({std::string()});
        {
            std::lock_guard<std::mutex> lock(cmdMutex);
            for(auto c : command){
                if(c!=' '){
                    tokens.back().push_back(c);
                } else
                if(!tokens.back().empty()){
                    tokens.push_back(std::string());
                }
            }
            send = false;
        }
        if(tokens.empty()) {return;}
        unsigned int level = 0;
        if(tokens[level]=="layers"){
            level++;
            if(tokens[level]=="add"){
                map.layers.push_back(std::vector<unsigned short>(map.width*map.height, 0));
            } else
            if(tokens[level]=="delete"){
                map.layers.erase(map.layers.begin()+std::stoi(tokens[++level]));
            } else
            if(tokens[level]=="change"){
                current_layer = std::stoi(tokens[++level]);
            } else
            if(tokens[level]=="clear"){
                if(std::ranges::all_of(tokens[++level], [](char c){return std::isdigit(c);})){
                    std::fill(map.layers[std::stoi(tokens[level])].begin(), map.layers[std::stoi(tokens[level])].end(), 0);
                } else {
                    for(auto& tile : map.layers[current_layer]){
                        tile = 0;
                    }
                }
            } else
            if(tokens[level++]=="texture"){
                if(tokens[level]=="list"){
                    for(size_t i = 0; i < map.textureLists.size(); i++){
                        Print(TextFormat("Texture List %d (target layer: %d):", i, map.textureLists[i].target_layer));
                        for(size_t j = 0; j < map.textureLists[i].textures.size(); j++){
                            Print(TextFormat("\t%d: %s", j, map.textureLists[i].textures[j].c_str()));
                        }
                    }
                } else
                if(tokens[level]=="add"){
                    TextureList tl;
                    if(auto it = std::find(tokens.begin(), tokens.end(), "-tl"); it!=tokens.end()){
                        if(it+1!=tokens.end()&&std::ranges::all_of(*(it+1), [](char c){return std::isdigit(c);})){
                            tl.target_layer = std::stoi(*it);
                        }
                    }
                    if(auto it = std::find(tokens.begin(), tokens.end(), "-ts"); it!=tokens.end()){
                        if(it+1!=tokens.end()){
                            for(auto c : *(it+1)){
                                if(c==','){
                                    tl.textures.push_back(std::string());
                                    continue;
                                }
                                if(!tl.textures.empty()){
                                    tl.textures.back().push_back(c);
                                } else {
                                    tl.textures.push_back(std::string(1, c));
                                }
                            }
                        }
                    }
                    map.textureLists.push_back(tl);
                } else
                if(tokens[level]=="remove"){
                    auto& textures = map.textureLists[std::stoi(tokens[++level])].textures;
                    textures.erase(std::remove(textures.begin(), textures.end(), tokens[++level]), textures.end());
                } else
                if(tokens[level]=="edit"){
                    unsigned short list_index = std::stoi(tokens[++level]);
                    auto& texturelist = map.textureLists[list_index];
                    if(auto it = std::find(tokens.begin(), tokens.end(), "-tl"); it!=tokens.end()){
                        if(it+1!=tokens.end()&&std::ranges::all_of(*(it+1), [](char c){return std::isdigit(c);})){
                            texturelist.target_layer = std::stoi(*it);
                        }
                    }
                    if(auto it = std::find(tokens.begin(), tokens.end(), "-ts-add"); it!=tokens.end()){
                        if(it+1!=tokens.end()&&std::ranges::all_of(*(it+1), [](char c){return std::isdigit(c);})){
                            texturelist.textures.push_back(*it);
                        }
                    }
                    if(auto it = std::find(tokens.begin(), tokens.end(), "-ts-remove"); it!=tokens.end()){
                        if(it+1!=tokens.end()&&std::ranges::all_of(*(it+1), [](char c){return std::isdigit(c);})){
                            texturelist.textures.erase(it+1);
                        }
                    }
                }
            }
        } else
        if(tokens[level]=="set"){
            level++;
            if(tokens[level]=="min_version"){
                map.min_version = tokens[++level];
                Print("Set min_version to " + tokens[level]);
            }
            if(tokens[level]=="max_version"){
                map.max_version = tokens[++level];
                Print("Set max_version to " + tokens[level]);
            }
        } else
        if(tokens[level]=="test-100-msgs"){
            for(int i = 0; i < 100; i++){
                Print("Test message " + std::to_string(i+1));
            }
        } else
        if(tokens[level]=="save"){
            saved = true;
        } else
        if(tokens[level]=="quit"||tokens[level]=="exit"){
            running = false;
        } else {
            Print("[ERROR]:Unknown command");
        }
    };
    while(running && !WindowShouldClose()){
        float width = GetScreenWidth();
        float height = GetScreenHeight();
        bool renamed = false;
        if(!IsReady){
            BeginDrawing();
            ClearBackground(BLACK);
            switch(state){
                case 1:
                {
                    bool NewBtn = GuiButton((Rectangle){
                        width*10.f/100.f, height*10.f/100.f,
                        200, 100
                    }, "New"),
                    LoadBtn = GuiButton((Rectangle){
                        width*10.f/100.f, height*10.f/100.f + 200,
                        200, 100
                    }, "Load");
                    if(NewBtn){state = 2;}
                    if(LoadBtn){state = 3;}
                    break;
                }
                case 2:
                {
                    if(error_state==1){
                        int respond = GuiMessageBox(
                            (Rectangle){width*25.f/100.f, height*25.f/100.f, width*50.f/100.f, height*50.f/100.f},
                            "Error", "File already exists", "Cancel;Replace;Rename"
                        );
                        if(respond>0){error_state = 0;}
                        if(respond==2){
                            InitMap(map, (path + file_name).c_str(), true);
                            IsReady = true;
                        } else if(respond==3){
                            std::filesystem::path base = path + file_name + ".json";
                            base.replace_extension(".json");
                            Print(TextFormat("Renaming %s", base.stem().string().c_str()));
                            std::filesystem::path dir = base.parent_path();
                            std::string stem = base.stem().string();
                            std::filesystem::path ext = base.extension().string();
                            std::filesystem::path candidate = base;
                            unsigned short subfix = 1;
                            while(std::filesystem::exists(candidate)){
                                Print("Checking if " + candidate.string() + " exists...");
                                candidate = dir / (stem + std::to_string(subfix++) + ext.string());
                            }
                            file_name = candidate.filename().string();
                            Print("Renamed file to: " + path + file_name);
                            renamed = true;
                        }
                    } else if(error_state==2){
                        GuiMessageBox(
                            (Rectangle){
                                width*25.f/100.f, height*25.f/100.f,
                                width*50.f/100.f, height*50.f/100.f
                            },
                            "Error", "Failed to load the file.", nullptr
                        );
                    }
                    bool BackBtn = GuiButton((Rectangle){
                        width*5.f/100.f, height*5.f/100.f,
                        50, 50
                    }, "<"),
                    InputTextBox = GuiTextBox((Rectangle){
                        width*20.f/100.f, height*10.f/100.f,
                        200, 50
                    }, file_name.data(), 40, editing[0]),
                    InputWidth = GuiValueBox((Rectangle){
                        width*20.f/100.f, height*10.f/100.f + 50*2,
                        200, 50
                    }, "Map Width", &map.width, 0, 1024, editing[1]),
                    InputHeight = GuiValueBox((Rectangle){
                        width*20.f/100.f, height*10.f/100.f + 50*4,
                        200, 50
                    }, "Map Height", &map.height, 0, 1024, editing[2]),
                    InputTileSize = GuiValueBox((Rectangle){
                        width*20.f/100.f, height*10.f/100.f + 50*6,
                        200, 50
                    }, "Tile Size", &map.tileSize, 0, 1024, editing[3]),
                    NewBtn = GuiButton((Rectangle){
                        width*75.f/100.f, height*75.f/100.f,
                        200, 50
                    }, "New");
                    if(BackBtn){state = 1;}
                    if(InputTextBox){
                        editing[0] = !editing[0];
                    }
                    if(InputWidth){
                        editing[1] = !editing[1];
                    }
                    if(InputHeight){
                        editing[2] = !editing[2];
                    }
                    if(InputTileSize){
                        editing[3] = !editing[3];
                    }
                    if(NewBtn||renamed){
                        if(file_name.empty()){
                            file_name = "Untitled";
                        }
                        error_state = InitMap(map, (path + file_name).c_str());
                        if(error_state!=1) {
                            IsReady = true;
                        } else {
                            printf("File already exists: %s.json\n", (path + file_name).c_str());
                        }
                    }
                    break;
                }
                case 3: IsReady = true; break;
                case 4: break;
                default: break;
            }
            EndDrawing();
            continue;
        }
        if(IsReady&&startedEditing==0){
            SaveMap(map, path+file_name);
            startedEditing = GetTime();
            lastAutoSave = startedEditing;
        }
        Parse();
        AutoSave(GetTime(), lastAutoSave, autoSaveTime, file_name, path, map);
        Vector2 worldMouse = GetScreenToWorld2D(GetMousePosition(), cam);
        bool MLB = IsMouseButtonDown(MOUSE_LEFT_BUTTON), MRB = IsMouseButtonDown(MOUSE_RIGHT_BUTTON);
        int tileX = worldMouse.x / map.tileSize;
        int tileY = worldMouse.y / map.tileSize;
        float wheel = GetMouseWheelMove(), frameDelta = GetFrameTime();
        if (wheel != 0&&cam.zoom < 5.f&&cam.zoom > 0.5f){
            float zoomFactor = 0.1f;
            cam.zoom += wheel * zoomFactor;
            if(cam.zoom < 0.5f){cam.zoom = 0.5f;}
            else if(cam.zoom > 5.f){cam.zoom = 5.f;}
        }
        if (IsMouseButtonDown(MOUSE_MIDDLE_BUTTON)){
            Vector2 delta = GetMouseDelta();
            delta = Vector2Scale(delta, -1.0f / cam.zoom);
            cam.target = Vector2Add(cam.target, delta);
        }
        if((MLB||MRB)&&!(MLB&&MRB)){
            if (tileX >= 0 && tileX < map.width &&
                tileY >= 0 && tileY < map.height){
                map.layers[current_layer][tileY * map.width + tileX] = MLB ? 1 : 0;
            }
        }
        if(((IsKeyDown(KEY_LEFT_CONTROL)||IsKeyDown(KEY_RIGHT_CONTROL))
        &&IsKeyPressed(KEY_S))||saved){
            SaveMap(map, path+file_name);
            saved = false;
        }
        if(IsKeyDown(KEY_LEFT)&&!IsKeyDown(KEY_RIGHT)){cam.target.x -= 75.f*frameDelta;}
        if(IsKeyDown(KEY_RIGHT)&&!IsKeyDown(KEY_LEFT)){cam.target.x += 75.f*frameDelta;}
        if(IsKeyDown(KEY_UP)&&!IsKeyDown(KEY_DOWN)){cam.target.y -= 75.f*frameDelta;}
        if(IsKeyDown(KEY_DOWN)&&!IsKeyDown(KEY_UP)){cam.target.y += 75.f*frameDelta;}
        if(IsKeyDown(KEY_Q)&&!IsKeyDown(KEY_E)&&cam.zoom>=0.1f){cam.zoom -= 1.f*frameDelta;}
        if(IsKeyDown(KEY_E)&&!IsKeyDown(KEY_Q)&&cam.zoom<=5.f){cam.zoom += 1.f*frameDelta;}
        BeginDrawing();
        ClearBackground(BLACK);
        BeginMode2D(cam);
        DrawGrid2D(cam, map.width, map.height, map.tileSize);
        for(int y = 0; y < map.height; y++){
            for(int x = 0; x < map.width; x++){
                if (map.layers[current_layer][y * map.width + x] == 1){
                    DrawRectangle(x*map.tileSize, y*map.tileSize, map.tileSize, map.tileSize, BLUE);
                }
            }
        }
        EndMode2D();
        EndDrawing();
    }
    CLI_Thread.join();
    CloseWindow();
    return 0;
}