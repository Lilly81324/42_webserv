/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   Server.cpp                                         :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: vvelikov <vvelikov@student.42.fr>          +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2025/08/08 22:15:43 by vvelikov          #+#    #+#             */
/*   Updated: 2025/08/09 15:39:15 by vvelikov         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#include "Server.hpp"
#include "Logger.hpp"

Server::Server(const Config &cfg) : _cfg(cfg){
	LOG_DEBUG("Server stub constructed.");
}

Server::~Server(){}

void Server::run(){
	LOG_DEBUG("Server::run() stub");
}