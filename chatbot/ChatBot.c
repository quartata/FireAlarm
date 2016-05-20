//
//  ChatBot.c
//  chatbot
//
//  Created on 5/5/16.
//  Copyright © 2016 NobodyNada. All rights reserved.
//

#include "ChatBot.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "ChatMessage.h"
#include "cJSON.h"
#include <zlib.h>

#define REPORT_HEADER "Potentially bad post"

ChatBot *createChatBot(ChatRoom *room, Command **commands, Filter **filters) {
    ChatBot *c = malloc(sizeof(ChatBot));
    c->room = room;
    c->commands = commands;
    c->runningCommands = NULL;
    c->apiFilter = NULL;
    c->runningCommandCount = 0;
    c->stopAction = ACTION_NONE;
    pthread_mutex_init(&c->runningCommandsLock, NULL);
    pthread_mutex_init(&c->detectorLock, NULL);
    
    c->filters = NULL;
    c->filterCount = 0;
    
    c->reportsWaiting = -1;
    
    while (*(filters++)) {
        c->filters = realloc(c->filters, ++c->filterCount * sizeof(Filter*));
        c->filters[c->filterCount-1] = *(filters - 1);
    }
    
    for (int i = 0; i < REPORT_MEMORY; i++) {
        c->latestReports[i] = NULL;
    }
    
    return c;
}

void runCommand(ChatBot *bot, ChatMessage *message, char *command) {
    //Get the space-separated components of this command.
    char **components = NULL;
    size_t componentCount = 0;
    
    char *component;
    while ((component = strsep(&command, " "))) {
        //add command to components
        components = realloc(components, (++componentCount) * sizeof(char*));
        components[componentCount-1] = malloc(strlen(component) + 1);
        strcpy(components[componentCount-1], component);
    };
    pthread_mutex_lock(&bot->runningCommandsLock);
    RunningCommand *c = launchCommand(message, (int)componentCount, components, bot->commands, bot);
    bot->runningCommands = realloc(bot->runningCommands, ++bot->runningCommandCount * sizeof(RunningCommand *));
    bot->runningCommands[bot->runningCommandCount-1] = c;
    pthread_mutex_unlock(&bot->runningCommandsLock);
}

void prepareCommand(ChatBot *bot, ChatMessage *message, char *messageText) {
    char *command = strchr(messageText, ' ');
    if (command) {
        while (*(++command) == ' ');
        if (*command && bot->stopAction == ACTION_NONE) {
            runCommand(bot, message, command);
            return;
        }
    }
}

Report *reportWithMessage(ChatBot *bot, unsigned long messageID) {
    for (int i = 0; i < REPORT_MEMORY; i++) {
        if (bot->latestReports[i]) {
            if (messageID == bot->latestReports[i]->messageID) {
                return bot->latestReports[i];
            }
        }
    }
    return NULL;
}

void processMessage(ChatBot *bot, ChatMessage *message) {
    char *messageText = malloc(strlen(message->content) + 1);
    strcpy(messageText, message->content);
    if (strstr(messageText, "@Bot") == messageText) {
        //messageText starts with "@Bot"
        prepareCommand(bot, message, messageText);
        
    }
    else if (bot->reportsWaiting != -1 && strstr(messageText, REPORT_HEADER)) {
        bot->latestReports[bot->reportsWaiting--]->messageID = message->id;
        deleteChatMessage(message);
    }
    else if (message->replyID && reportWithMessage(bot, message->replyID)) {
        prepareCommand(bot, message, messageText);
    }
    else {
        deleteChatMessage(message);
    }
    free(messageText);
}

Post *getPostByID(ChatBot *bot, unsigned long postID) {
    pthread_mutex_lock(&bot->room->clientLock);
    CURL *curl = bot->room->client->curl;
    
    checkCURL(curl_easy_setopt(curl, CURLOPT_HTTPGET, 1));
    OutBuffer buffer;
    buffer.data = NULL;
    checkCURL(curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer));
    
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip, deflate");
    
    if (bot->apiFilter == NULL) {
        checkCURL(curl_easy_setopt(curl, CURLOPT_URL,
                                   "api.stackexchange.com/2.2/filters/create?include=post.title;post.body;question.tags&unsafe=false"
                                   ));
        checkCURL(curl_easy_perform(curl));
        
        cJSON *json = cJSON_Parse(buffer.data);
        free(buffer.data);
        buffer.data = NULL;
        
        cJSON *items = cJSON_GetObjectItem(json, "items");
        bot->apiFilter = cJSON_GetObjectItem(cJSON_GetArrayItem(items, 0), "filter")->valuestring;
    }
    
    
    
    unsigned max = 256;
    char request[max];
    snprintf(request, max,
             "https://api.stackexchange.com/posts/%lu?site=stackoverflow&filter=%s",
             postID, bot->apiFilter
             );
    curl_easy_setopt(curl, CURLOPT_URL, request);
    
    
    
    checkCURL(curl_easy_perform(curl));
    
    checkCURL(curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""));
    
    
    pthread_mutex_unlock(&bot->room->clientLock);
    
    cJSON *json = cJSON_Parse(buffer.data);
    free(buffer.data);
    
    if (cJSON_GetObjectItem(json, "error_id")) {
        cJSON_Delete(json);
        puts("Error fetching post!");
        return NULL;
    }
    
    cJSON *postJSON = cJSON_GetArrayItem(cJSON_GetObjectItem(json, "items"), 0);
    if (postJSON == NULL) {
        cJSON_Delete(json);
        return NULL;
    }
    
    //puts(cJSON_Print(postJSON));
    
    
    char *title = cJSON_GetObjectItem(postJSON, "title")->valuestring;
    char *body = cJSON_GetObjectItem(postJSON, "body")->valuestring;
    char *type = cJSON_GetObjectItem(postJSON, "post_type")->valuestring;
    unsigned userID = cJSON_GetObjectItem(cJSON_GetObjectItem(postJSON, "owner"), "user_id")->valueint;
    
    Post *p = createPost(title, body, postID, strcmp(type, "answer") == 0, userID);
    
    cJSON_Delete(json);
    return p;
}

void checkPost(ChatBot *bot, Post *post) {
    unsigned char match = 0;
    char *messageBuf = malloc(sizeof(char));
    *messageBuf = 0;
    for (int i = 0; i < bot->filterCount; i++) {
        unsigned start, end;
        if (postMatchesFilter(post, bot->filters[i], &start, &end)) {
            
            const char *desc = bot->filters[i]->desc;
            messageBuf = realloc(messageBuf, strlen(messageBuf) + strlen(desc) + 1);
            
            snprintf(messageBuf + strlen(messageBuf), strlen(desc) + 16,
                     "%s%s", desc, (i < bot->filterCount - 1 ? ", " : ""));
            
            match = 1;
        }
    }
    if (match) {
        const size_t maxMessage = strlen(messageBuf) + 256;
        char *message = malloc(maxMessage);
        snprintf(message, maxMessage,
                 REPORT_HEADER " (%s): [%s](http://stackoverflow.com/%s/%lu)",
                 messageBuf, post->title, post->isAnswer ? "a" : "q", post->postID);
        
        postMessage(bot->room, message);
        
        if (bot->latestReports[REPORT_MEMORY-1]) {
            free(bot->latestReports[REPORT_MEMORY-1]->post);
            free(bot->latestReports[REPORT_MEMORY-1]);
        }
        int i = REPORT_MEMORY;
        while(--i) {
            bot->latestReports[i] = bot->latestReports[i-1];
        }
        Report *report = malloc(sizeof(Report));
        report->post = post;
        bot->latestReports[0] = report;
        bot->reportsWaiting++;
        free(message);
    }
    else {
        free(post);
    }
    
    free(messageBuf);
}

void confirmPost(ChatBot *bot, Post *post, unsigned char confirmed) {
    for (int i = 0; i < bot->filterCount; i++) {
        Filter *filter = bot->filters[i];
        if (postMatchesFilter(post, filter, NULL, NULL)) {
            if (confirmed) {
                filter->truePositives++;
            }
            else {
                filter->falsePositives++;
            }
            printf("%s true positive count of %s.\n", confirmed ? "Increased" : "Decreased", filter->desc);
        }
    }
}

StopAction runChatBot(ChatBot *c) {
    ChatMessage **messages = processChatRoomEvents(c->room);
    ChatMessage *message;
    for (int i = 0; (message = messages[i]); i++) {
        processMessage(c, message);
    }
    free(messages);
    
    //clean up old commands
    for (int i = 0; i < c->runningCommandCount; i++) {
        if (c->runningCommands[i]->finished) {
            //delete the command
            c->runningCommandCount--;
            int j = i;
            for (deleteRunningCommand(c->runningCommands[j]); j < c->runningCommandCount; j++) {
                c->runningCommands[i] = c->runningCommands[i+1];
            }
            c->runningCommands = realloc(c->runningCommands, c->runningCommandCount * sizeof(RunningCommand*));
        }
    }
    if (c->stopAction != ACTION_NONE) {
        if (c->room->pendingMessageLinkedList == NULL && (c->runningCommandCount == 0)) {
            return c->stopAction;
        }
    }
    
    return ACTION_NONE;
}
