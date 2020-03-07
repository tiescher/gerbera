/*GRB*
  Gerbera - https://gerbera.io/

  search_handler.cc - this file is part of Gerbera.

  Copyright (C) 2018 Gerbera Contributors

  Gerbera is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License version 2
  as published by the Free Software Foundation.

  Gerbera is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Gerbera.  If not, see <http://www.gnu.org/licenses/>.

  $Id$
*/
#include "search_handler.h"
#include "config/config_manager.h"
#include "storage/storage.h"
#include "util/tools.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <stack>

static std::unordered_map<std::string, TokenType> tokenTypes {
    { "(", TokenType::LPAREN },
    { ")", TokenType::RPAREN },
    { "*", TokenType::ASTERISK },
    { "\"", TokenType::DQUOTE },
    { "true", TokenType::BOOLVAL },
    { "false", TokenType::BOOLVAL },
    { "exists", TokenType::EXISTS },
    { "contains", TokenType::STRINGOP },
    { "doesnotcontain", TokenType::STRINGOP },
    { "derivedfrom", TokenType::STRINGOP },
    { "startswith", TokenType::STRINGOP },
    { "=", TokenType::COMPAREOP },
    { "!=", TokenType::COMPAREOP },
    { "<", TokenType::COMPAREOP },
    { "<=", TokenType::COMPAREOP },
    { ">", TokenType::COMPAREOP },
    { ">=", TokenType::COMPAREOP },
    { "and", TokenType::AND },
    { "or", TokenType::OR }
};

static std::string aslowercase(const std::string& src)
{
    std::string copy = src;
    std::transform(copy.begin(), copy.end(), copy.begin(), ::tolower);
    return copy;
}

std::unique_ptr<SearchToken> SearchLexer::nextToken()
{
    for (; currentPos < input.length();) {
        char ch = input[currentPos];

        switch (ch) {
        case '(':
        case ')':
        case '*':
        case '=': {
            auto token = std::string(&ch, 1);
            TokenType tokenType = tokenTypes.at(token);
            currentPos++;
            return std::make_unique<SearchToken>(tokenType, token);
        }
        case '>':
        case '<':
        case '!':
            if (input[currentPos + 1] == '=') {
                auto token = std::string(&ch, 1);
                token.push_back('=');
                TokenType tokenType = tokenTypes.at(token);
                currentPos += 2;
                return std::make_unique<SearchToken>(tokenType, token);
            } else {
                auto token = std::string(&ch, 1);
                TokenType tokenType = tokenTypes.at(token);
                currentPos++;
                return std::make_unique<SearchToken>(tokenType, token);
            }
        case '"':
            if (!inQuotes) {
                auto token = std::string(&ch, 1);
                currentPos++;
                inQuotes = true;
                return std::make_unique<SearchToken>(TokenType::DQUOTE, std::move(token));
            } else {
                auto token = std::string(&ch, 1);
                currentPos++;
                inQuotes = false;
                return std::make_unique<SearchToken>(TokenType::DQUOTE, std::move(token));
            }
        default:
            if (inQuotes) {
                auto quotedStr = getQuotedValue(input);
                if (quotedStr.length())
                    return std::make_unique<SearchToken>(TokenType::ESCAPEDSTRING, std::move(quotedStr));
            }
            if (std::isspace(ch)) {
                currentPos++;
            } else {
                auto tokenStr = nextStringToken(input);
                if (tokenStr.length()) {
                    std::unique_ptr<SearchToken> token = makeToken(tokenStr);
                    if (token->getValue().length())
                        return token;
                }
            }
        }
    }
    return nullptr;
}

std::string SearchLexer::getQuotedValue(const std::string& input)
{
    std::string token;
    bool escaping = false;
    for (; currentPos < input.length();) {
        auto ch = input[currentPos];
        if (ch == '"' && !escaping) {
            break;
        }
        if (!escaping && ch == '\\') {
            escaping = true;
        } else {
            token.push_back(ch);
            if (ch != '\\')
                escaping = false;
        }
        currentPos++;
    }
    return token;
}

std::string SearchLexer::nextStringToken(const std::string& input)
{
    auto startPos = currentPos;
    for (; currentPos < input.length();) {
        auto ch = input[currentPos];
        if (std::isalnum(ch) || ch == ':' || ch == '@' || ch == '.')
            currentPos++;
        else
            break;
    }
    return input.substr(startPos, currentPos - startPos);
}

std::unique_ptr<SearchToken> SearchLexer::makeToken(const std::string& tokenStr)
{
    auto itr = tokenTypes.find(aslowercase(tokenStr));
    if (itr != tokenTypes.end()) {
        return std::make_unique<SearchToken>(itr->second, tokenStr);
    }
    return std::make_unique<SearchToken>(TokenType::PROPERTY, tokenStr);
}

void SearchParser::getNextToken()
{
    currentToken = lexer->nextToken();
}

std::shared_ptr<ASTNode> SearchParser::parse()
{
    getNextToken();
    if (currentToken->getType() == TokenType::ASTERISK)
        return std::make_shared<ASTAsterisk>(sqlEmitter, currentToken->getValue());

    return parseSearchExpression();
}

std::shared_ptr<ASTNode> SearchParser::parseSearchExpression()
{
    std::stack<std::shared_ptr<ASTNode>> nodeStack;
    std::stack<TokenType> operatorStack;
    std::shared_ptr<ASTNode> root = nullptr;
    std::shared_ptr<ASTNode> expressionNode = nullptr;
    TokenType currentOperator = TokenType::INVALID;
    while (currentToken) {
        if (currentToken->getType() == TokenType::PROPERTY) {
            expressionNode = parseRelationshipExpression();
            if (currentOperator == TokenType::AND) {
                if (nodeStack.top() == nullptr)
                    throw std::runtime_error("Cannot construct ASTAndOperator without lhs");
                if (expressionNode == nullptr)
                    throw std::runtime_error("Cannot construct ASTAndOperator without rhs");
                std::shared_ptr<ASTNode> lhs(nodeStack.top());
                nodeStack.pop();
                nodeStack.push(std::make_shared<ASTAndOperator>(sqlEmitter, lhs, expressionNode));
                operatorStack.pop();
            } else if (currentOperator == TokenType::OR) {
                if (nodeStack.top() == nullptr)
                    throw std::runtime_error("Cannot construct ASTOrOperator without lhs");
                if (expressionNode == nullptr)
                    throw std::runtime_error("Cannot construct ASTOrOperator without rhs");
                std::shared_ptr<ASTNode> lhs(nodeStack.top());
                nodeStack.pop();
                nodeStack.push(std::make_shared<ASTOrOperator>(sqlEmitter, lhs, expressionNode));
                operatorStack.pop();
            } else {
                nodeStack.push(expressionNode);
            }
            currentOperator = TokenType::INVALID;
            getNextToken();
        } else if (currentToken->getType() == TokenType::LPAREN) {
            nodeStack.push(parseParenthesis());
            getNextToken();
        } else if (currentToken->getType() == TokenType::AND || currentToken->getType() == TokenType::OR) {
            currentOperator = currentToken->getType();
            operatorStack.push(currentOperator);
            getNextToken();
        }
    }

    while (!nodeStack.empty()) {
        root = nodeStack.top();
        nodeStack.pop();
        if (!operatorStack.empty()) {
            currentOperator = operatorStack.top();
            operatorStack.pop();
            if (!nodeStack.empty()) {
                std::shared_ptr<ASTNode> lhs = nodeStack.top();
                nodeStack.pop();
                if (currentOperator == TokenType::AND)
                    root = std::make_shared<ASTAndOperator>(sqlEmitter, lhs, root);
                else
                    root = std::make_shared<ASTOrOperator>(sqlEmitter, lhs, root);
            } else
                throw std::runtime_error("Cannot construct ASTOrOperator/ASTAndOperator without rhs");
        }
    }
    return root;
}

std::shared_ptr<ASTNode> SearchParser::parseParenthesis()
{
    if (currentToken->getType() != TokenType::LPAREN)
        throw std::runtime_error("Failed to parse search criteria - expecting a ')'");

    std::shared_ptr<ASTNode> currentNode = nullptr;
    std::shared_ptr<ASTNode> lhsNode = nullptr;
    std::shared_ptr<ASTNode> rhsNode = nullptr;
    getNextToken();
    while (currentToken != nullptr && currentToken->getType() != TokenType::RPAREN) {
        // just call parseSearchExpression() at this point?
        if (currentToken->getType() == TokenType::PROPERTY) {
            currentNode = parseRelationshipExpression();
            getNextToken();
        } else if (currentToken->getType() == TokenType::AND || currentToken->getType() == TokenType::OR) {
            auto tokenType = currentToken->getType();
            lhsNode = currentNode;

            getNextToken();
            if (currentToken->getType() == TokenType::LPAREN)
                rhsNode = parseParenthesis();
            else
                rhsNode = parseRelationshipExpression();

            if (tokenType == TokenType::AND)
                currentNode = std::make_shared<ASTAndOperator>(sqlEmitter, lhsNode, rhsNode);
            else if (tokenType == TokenType::OR)
                currentNode = std::make_shared<ASTOrOperator>(sqlEmitter, lhsNode, rhsNode);
            else
                throw std::runtime_error("Failed to parse search criteria - expected and/or");

            getNextToken();
        } else if (currentToken->getType() == TokenType::LPAREN) {
            currentNode = parseParenthesis();
            getNextToken();
        }
    }
    if (currentNode == nullptr)
        throw std::runtime_error("Failed to parse search criteria - bad expression between parenthesis");

    return std::make_shared<ASTParenthesis>(sqlEmitter, currentNode);
}

std::shared_ptr<ASTNode> SearchParser::parseRelationshipExpression()
{
    if (currentToken->getType() != TokenType::PROPERTY)
        throw std::runtime_error("Failed to parse search criteria - expecting a property name");

    std::shared_ptr<ASTNode> relationshipExpr = nullptr;
    std::shared_ptr<ASTProperty> property = std::make_shared<ASTProperty>(sqlEmitter, currentToken->getValue());

    getNextToken();
    if (currentToken->getType() == TokenType::COMPAREOP) {
        auto operatr = std::make_shared<ASTCompareOperator>(sqlEmitter, currentToken->getValue());
        getNextToken();
        auto quotedString = parseQuotedString();
        relationshipExpr = std::make_shared<ASTCompareExpression>(sqlEmitter, property, operatr, quotedString);
    } else if (currentToken->getType() == TokenType::STRINGOP) {
        auto operatr = std::make_shared<ASTStringOperator>(sqlEmitter, currentToken->getValue());
        getNextToken();
        auto quotedString = parseQuotedString();
        relationshipExpr = std::make_shared<ASTStringExpression>(sqlEmitter, property, operatr, quotedString);
    } else if (currentToken->getType() == TokenType::EXISTS) {
        auto operatr = std::make_shared<ASTExistsOperator>(sqlEmitter, currentToken->getValue());
        getNextToken();
        auto booleanValue = std::make_shared<ASTBoolean>(sqlEmitter, currentToken->getValue());
        relationshipExpr = std::make_shared<ASTExistsExpression>(sqlEmitter, property, operatr, booleanValue);
    } else
        throw std::runtime_error("Failed to parse search criteria - expecting a comparison, exists, or string operator");

    return relationshipExpr;
}

std::shared_ptr<ASTQuotedString> SearchParser::parseQuotedString()
{
    if (currentToken->getType() != TokenType::DQUOTE)
        throw std::runtime_error("Failed to parse search criteria - expecting a double-quote");
    auto openQuote = std::make_shared<ASTDQuote>(sqlEmitter, currentToken->getValue());
    getNextToken();

    if (currentToken->getType() != TokenType::ESCAPEDSTRING)
        throw std::runtime_error("Failed to parse search criteria - expecting an escaped string value");

    auto escapedString = std::make_shared<ASTEscapedString>(sqlEmitter, currentToken->getValue());
    getNextToken();

    if (currentToken->getType() != TokenType::DQUOTE)
        throw std::runtime_error("Failed to parse search criteria - expecting a double-quote");
    auto closeQuote = std::make_shared<ASTDQuote>(sqlEmitter, currentToken->getValue());

    return std::make_shared<ASTQuotedString>(sqlEmitter, openQuote, escapedString, closeQuote);
}

void SearchParser::checkIsExpected(TokenType tokenType, const std::string& tokenTypeDescription)
{
    if (currentToken->getType() != tokenType) {
        std::string errorMsg(std::string("Failed to parse search criteria - expecting ") + tokenTypeDescription);
        throw std::runtime_error(errorMsg.c_str());
    }
}

std::string ASTNode::emitSQL() const
{
    return sqlEmitter.emitSQL(this);
}

std::string ASTAsterisk::emit() const
{
    return sqlEmitter.emit(this);
}

std::string ASTProperty::emit() const
{
    return value;
}

std::string ASTBoolean::emit() const
{
    return value;
}

std::string ASTParenthesis::emit() const
{
    return sqlEmitter.emit(this, bracketedNode->emit());
}

std::string ASTDQuote::emit() const
{
    return sqlEmitter.emit(this);
}

std::string ASTEscapedString::emit() const
{
    return value;
}

std::string ASTQuotedString::emit() const
{
    return openQuote->emit() + escapedString->emit() + closeQuote->emit();
}

std::string ASTCompareOperator::emit() const
{
    throw std::runtime_error("Should not get here");
}

std::string ASTCompareOperator::emit(const std::string& property, const std::string& value) const
{
    return sqlEmitter.emit(this, property, value);
}

std::string ASTCompareExpression::emit() const
{
    return operatr->emit(lhs->emit(), rhs->emit());
}

std::string ASTStringOperator::emit() const
{
    throw std::runtime_error("Should not get here");
}

std::string ASTStringOperator::emit(const std::string& property, const std::string& value) const
{
    return sqlEmitter.emit(this, property, value);
}

std::string ASTStringExpression::emit() const
{
    return operatr->emit(lhs->emit(), rhs->emit());
}

std::string ASTExistsOperator::emit() const
{
    std::cout << "Emitting for ASTExistsOperator " << std::endl;
    throw std::runtime_error("Should not get here");
}

std::string ASTExistsOperator::emit(const std::string& property, const std::string& value) const
{
    return sqlEmitter.emit(this, property, value);
}

std::string ASTExistsExpression::emit() const
{
    return operatr->emit(lhs->emit(), rhs->emit());
}

std::string ASTAndOperator::emit() const
{
    return sqlEmitter.emit(this, lhs->emit(), rhs->emit());
}

std::string ASTOrOperator::emit() const
{
    return sqlEmitter.emit(this, lhs->emit(), rhs->emit());
}

std::string DefaultSQLEmitter::emitSQL(const ASTNode* node) const
{
    std::string predicates = node->emit();
    if (predicates.length() > 0) {
        std::ostringstream sql;
        sql << "from mt_cds_object c "
            << "inner join mt_metadata m on c.id = m.item_id "
            << "where "
            << predicates;
        return sql.str();
    }
    throw std::runtime_error("No SQL generated from AST");
}

std::string DefaultSQLEmitter::emit(const ASTParenthesis* node, const std::string& bracketedNode) const
{
    std::ostringstream sqlFragment;
    sqlFragment << "(" << bracketedNode << ")";
    return sqlFragment.str();
}

std::string DefaultSQLEmitter::emit(const ASTCompareOperator* node, const std::string& property,
    const std::string& value) const
{
    auto operatr = node->getValue();
    if (operatr != "=")
        throw std::runtime_error("operator not yet supported");

    std::ostringstream sqlFragment;
    sqlFragment << "(m.property_name='" << property << "' and lower(m.property_value)"
                << operatr << "lower('" << value << "') and c.upnp_class is not null)";
    return sqlFragment.str();
}

std::string DefaultSQLEmitter::emit(const ASTStringOperator* node, const std::string& property,
    const std::string& value) const
{
    auto lcOperator = aslowercase(node->getValue());
    if (lcOperator != "contains" && lcOperator != "doesnotcontain" && lcOperator != "derivedfrom"
        && lcOperator != "startswith")
        throw std::runtime_error("operator not supported");

    std::ostringstream sqlFragment;
    if (lcOperator == "contains") {
        sqlFragment << "(m.property_name='" << property << "' and lower(m.property_value) "
                    << "like"
                    << " lower('%" << value << "%') and c.upnp_class is not null)";
    } else if (lcOperator == "doesnotcontain") {
        sqlFragment << "(m.property_name='" << property << "' and lower(m.property_value) "
                    << "not like"
                    << " lower('%" << value << "%') and c.upnp_class is not null)";
    } else if (lcOperator == "startswith") {
        sqlFragment << "(m.property_name='" << property << "' and lower(m.property_value) "
                    << "like"
                    << " lower('" << value << "%') and c.upnp_class is not null)";
    } else if (lcOperator == "derivedfrom") {
        sqlFragment << "c.upnp_class "
                    << "like"
                    << " lower('" << value << ".%')";
    }
    return sqlFragment.str();
}

std::string DefaultSQLEmitter::emit(const ASTExistsOperator* node, const std::string& property,
    const std::string& value) const
{
    std::ostringstream sqlFragment;
    std::string exists;
    if (value == "true") {
        exists = "not null";
    } else if (value == "false") {
        exists = "null";
    } else {
        throw std::runtime_error("invalid value on rhs of exists operator");
    }
    sqlFragment << "(m.property_name='" << property << "' and m.property_value is " << exists << " and c.upnp_class is not null)";
    return sqlFragment.str();
}

std::string DefaultSQLEmitter::emit(const ASTAndOperator* node, const std::string& lhs,
    const std::string& rhs) const
{
    std::ostringstream sqlFragment;
    sqlFragment << lhs << " and " << rhs;
    return sqlFragment.str();
}

std::string DefaultSQLEmitter::emit(const ASTOrOperator* node, const std::string& lhs,
    const std::string& rhs) const
{
    std::ostringstream sqlFragment;
    sqlFragment << lhs << " or " << rhs;
    return sqlFragment.str();
}
